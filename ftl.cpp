#include "ftl.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <fstream>

/* ---------------- FTL ---------------- */
FTL::FTL(NandDriver &drv, NandRuntime &rt, BlockManager &alloc, int total_lbas)
    : nand_drive(drv), nand_runtime(rt), block_manager(alloc)
{
    seq_ = 1;
    total_pages_ = drv.pages_per_block() * drv.blocks_per_plane() * drv.planes_per_die() * drv.dies_per_nand();
    L2P.assign(total_lbas, -1);
    P2L.assign(total_pages_, -1);
    pstate.assign(total_pages_, PageState::EMPTY);

    // BBT from OOB
    for (int d = 0; d < nand_drive.dies_per_nand(); ++d)
        for (int p = 0; p < nand_drive.planes_per_die(); ++p)
            for (int b = 0; b < nand_drive.blocks_per_plane(); ++b)
                nand_runtime.bad_block_table[nand_runtime.idx(d, p, b)] = nand_drive.is_block_bad(d, p, b);

    // Allocator init (含 FACTORY BAD BLOCK remap)
    block_manager.init_from_bbt([this](int d, int p, int b)
                                { return nand_drive.is_block_bad(d, p, b); });
}

void FTL::write(int lba, const string &data)
{
    if (lba < 0 || lba >= (int)L2P.size())
    {
        cerr << "bad LBA\n";
        return;
    }
    if (L2P[lba] != -1)
    {
        int old = L2P[lba];
        pstate[old] = PageState::INVALID;
        P2L[old] = -1;
        L2P[lba] = -1;
    }
    int pba = -1;
    // 简单：遍历全 plane 分配一页
    for (int d = 0; d < nand_drive.dies_per_nand() && pba == -1; ++d)
        for (int p = 0; p < nand_drive.planes_per_die() && pba == -1; ++p)
            pba = block_manager.alloc_page(d, p);
    if (pba == -1)
    {
        run_gc();
        for (int d = 0; d < nand_drive.dies_per_nand() && pba == -1; ++d)
            for (int p = 0; p < nand_drive.planes_per_die() && pba == -1; ++p)
                pba = block_manager.alloc_page(d, p);
        if (pba == -1)
        {
            cerr << "no space after GC\n";
            return;
        }
    }
    if (!program_pba_with_handling(pba, data, lba))
    {
        cerr << "program fail\n";
        return;
    }
    L2P[lba] = pba;
    P2L[pba] = lba;
    pstate[pba] = PageState::VALID;
}

void FTL::read(int lba)
{
    if (lba < 0 || lba >= (int)L2P.size())
    {
        cerr << "bad LBA\n";
        return;
    }
    int pba = L2P[lba];
    if (pba == -1 || pstate[pba] != PageState::VALID)
    {
        cerr << "unmapped\n";
        return;
    }
    auto [d, p, b, g] = idx_from_pba(pba);
    NandOp op;
    op.cmd = NandCmd::READ_PAGE;
    op.targets.push_back({d, p, b, g});
    auto r = nand_drive.submit(op);
    if (r.first == NandStatus::SUCCESS && !op.data.empty())
        cout << setw(6) << op.data[0] << " ";
    else
        cerr << "read failed\n";
}

void FTL::dump_page_stats()
{
    for (int i = 0; i < total_pages_; i++)
    {
        auto [d, p, b, g] = idx_from_pba(i);
        if (nand_runtime.bad_block_table[nand_runtime.idx(d, p, b)])
        {
            // 如果是坏块，输出 B
            cout << "B" << " ";
            if (i % nand_drive.pages_per_block() == (nand_drive.pages_per_block()-1))
            {
                cout << "\n";
            }
            continue;
        }
        if (pstate[i] == PageState::VALID)
        {
            cout << "V" << " ";
        }
        else if (pstate[i] == PageState::INVALID)
        {
            cout << "I" << " ";
        }
        else
        {
            cout << "E" << " ";
        }
        if (i % nand_drive.pages_per_block() == (nand_drive.pages_per_block()-1))
        {
            cout << "\n";
        }
    }
}

void FTL::dump_stats()
{
    int V = 0, I = 0, E = 0;
    for (auto s : pstate)
    {
        if (s == PageState::VALID)
            V++;
        else if (s == PageState::INVALID)
            I++;
        else
            E++;
    }
    uint32_t min_ec = UINT32_MAX, max_ec = 0;
    int bad = 0;
    for (int d = 0; d < nand_drive.dies_per_nand(); ++d)
        for (int p = 0; p < nand_drive.planes_per_die(); ++p)
            for (int b = 0; b < nand_drive.blocks_per_plane(); ++b)
            {
                if (nand_runtime.bad_block_table[nand_runtime.idx(d, p, b)])
                    {
                        bad++;
                        continue;
                    }
                auto ec = nand_runtime.erase_count[nand_runtime.idx(d, p, b)];
                min_ec = min(min_ec, ec);
                max_ec = max(max_ec, ec);
            }
    cout << "[STATS] V=" << V << " I=" << I << " E=" << E
         << " bad_blocks=" << bad << " erase_gap=" << (int)(max_ec - min_ec) << " min_ec=" << min_ec << " max_ec=" << max_ec << "\n";

    // 输出NAND驱动统计信息
    const auto &nand_stats = nand_drive.get_stats();
    cout << "[NAND STATS] READ=" << nand_stats.read_ops
         << " PROGRAM=" << nand_stats.program_ops
         << " ERASE=" << nand_stats.erase_ops
         << " FAILED=" << nand_stats.failed_ops
         << " BAD_BLOCKS=" << nand_stats.bad_blocks_detected << "\n";
}

bool FTL::program_pba_with_handling(int &pba, const string &data, int lba)
{
    auto [d, p, b, g] = idx_from_pba(pba);
    if (nand_runtime.bad_block_table[nand_runtime.idx(d, p, b)])
        return false;
    NandOp op;
    op.cmd = NandCmd::PROGRAM_PAGE;
    op.targets.push_back({d, p, b, g});
    op.data.push_back(data);
    op.oob_lba.push_back(lba);
    op.oob_seq.push_back(seq_++);
    auto r = nand_drive.submit(op);
    if (r.first == NandStatus::SUCCESS)
        return true;

    // 写失败 => 块判坏：标 OOB, BBT 置位，Allocator 做 BAD BLOCK TABLE remap
    nand_drive.mark_block_bad_oob(d, p, b);
    nand_runtime.bad_block_table[nand_runtime.idx(d, p, b)] = true;
    // 失效该块所有页（保守处理）
    int start = pba_from_indices(d, p, b, 0);
    for (int gg = 0; gg < nand_drive.pages_per_block(); ++gg)
    {
        int x = start + gg;
        if (pstate[x] == PageState::VALID)
        {
            int l = P2L[x];
            if (l >= 0)
                L2P[l] = -1;
        }
        pstate[x] = PageState::INVALID;
        P2L[x] = -1;
    }
    // 通知分配器：坏 PBN -> remap 到一个 spare
    bool ok = block_manager.remap_grown_bad(d, p, b);
    if (ok)
    {
        std::cout << "remap bad block\n";
    }
    // 丢弃 open（如果正好写这个块）
    block_manager.drop_open_if_matches(d, p, b, true);

    // 重新申请一个页再写一次
    int np = -1;
    for (int dd = 0; dd < nand_drive.dies_per_nand() && np == -1; ++dd)
        for (int pp = 0; pp < nand_drive.planes_per_die() && np == -1; ++pp)
            np = block_manager.alloc_page(dd, pp);
    if (np == -1)
        return false;

    auto [d2, p2, b2, g2] = idx_from_pba(np);
    pba = pba_from_indices(d2, p2, b2, g2);
    NandOp op2;
    op2.cmd = NandCmd::PROGRAM_PAGE;
    op2.targets.push_back({d2, p2, b2, g2});
    op2.data.push_back(data);
    op2.oob_lba.push_back(lba);
    op2.oob_seq.push_back(seq_++);
    return nand_drive.submit(op2).first == NandStatus::SUCCESS;
}

void FTL::erase_block_txn(int d, int p, int b /*PBN*/)
{
    if (nand_runtime.bad_block_table[nand_runtime.idx(d, p, b)])
        return;
    NandOp op;
    op.cmd = NandCmd::ERASE_BLOCK;
    op.targets.push_back({d, p, b, -1});
    auto r = nand_drive.submit(op);
    if (r.first != NandStatus::SUCCESS)
    {
        // 擦除失败 => 块坏
        nand_drive.mark_block_bad_oob(d, p, b);
        nand_runtime.bad_block_table[nand_runtime.idx(d, p, b)] = true;
        block_manager.remap_grown_bad(d, p, b);
    }
    int start = pba_from_indices(d, p, b, 0);
    for (int g = 0; g < nand_drive.pages_per_block(); ++g)
    {
        pstate[start + g] = PageState::EMPTY;
        P2L[start + g] = -1;
    }
    block_manager.on_erase_complete(d, p, b);
}

void FTL::run_gc()
{
    // cout << "[GC] start\n";
    int vd = -1, vp = -1, vb = -1, min_valid = INT_MAX;
    for (int d = 0; d < nand_drive.dies_per_nand(); ++d)
    {
        for (int p = 0; p < nand_drive.planes_per_die(); ++p)
        {
            // 选 victim：按 **PBN** 统计有效页数量（VBN->PBN 后统计）
            for (int v = 0; v < nand_drive.blocks_per_plane(); ++v)
            {
                int b = drv_vbn_to_pbn(d, p, v);
                if (nand_runtime.bad_block_table[nand_runtime.idx(d, p, b)])
                    continue;
                // 避免选中当前 open
                // （allocator 内部有 open_vbn，我们简单通过 pba 比较规避）
                int start = pba_from_indices(d, p, b, 0);
                int valid = 0;
                for (int g = 0; g < nand_drive.pages_per_block(); ++g)
                    if (pstate[start + g] == PageState::VALID)
                        valid++;
                if (valid < min_valid)
                {
                    min_valid = valid;
                    vd = d;
                    vp = p;
                    vb = b;
                }
            }
        }
    }
    if (vb == -1)
    {
        cerr << "[GC] no victim\n";
        return;
    }

    int start = pba_from_indices(vd, vp, vb, 0);
    for (int g = 0; g < nand_drive.pages_per_block(); ++g)
    {
        int oldp = start + g;
        if (pstate[oldp] == PageState::VALID)
        {
            int l = P2L[oldp];
            if (l < 0)
                continue;
            int np = -1;
            for (int dd = 0; dd < nand_drive.dies_per_nand() && np == -1; ++dd)
                for (int pp = 0; pp < nand_drive.planes_per_die() && np == -1; ++pp)
                    np = block_manager.alloc_page(dd, pp);
            if (np == -1)
            {
                cerr << "[GC] alloc fail\n";
                return;
            }
            // auto [d2, p2, b2, g2] = idx_from_pba(np);
            NandOp op;
            op.cmd = NandCmd::READ_PAGE;
            op.targets.push_back({vd, vp, vb, g});
            auto r = nand_drive.submit(op);
            if (!program_pba_with_handling(np, op.data[0], l))
            {
                cerr << "[GC] prog fail\n";
                continue;
            }
            pstate[np] = PageState::VALID;
            P2L[np] = l;
            L2P[l] = np;
            pstate[oldp] = PageState::INVALID;
            P2L[oldp] = -1;
        }
    }
    erase_block_txn(vd, vp, vb);
    // cout << "[GC] done\n";
}

// helpers
int FTL::drv_vbn_to_pbn(int d, int p, int vbn)
{
    // 通过 allocator 的公开接口解析
    return block_manager.resolve_pbn(d, p, vbn);
}

int FTL::pages_per_plane() const { return nand_drive.pages_per_block() * nand_drive.blocks_per_plane(); }
int FTL::pages_per_die() const { return pages_per_plane() * nand_drive.planes_per_die(); }

int FTL::pba_from_indices(int d, int p, int b, int g) const
{
    return d * pages_per_die() + p * pages_per_plane() + b * nand_drive.pages_per_block() + g;
}

tuple<int, int, int, int> FTL::idx_from_pba(int pba) const
{
    int ppb = nand_drive.pages_per_block(), bpp = nand_drive.blocks_per_plane(), ppd = nand_drive.planes_per_die();
    int pplane = ppb * bpp, pdie = pplane * ppd;
    int d = pba / pdie, p = (pba % pdie) / pplane, b = (pba % pplane) / ppb, g = pba % ppb;
    return {d, p, b, g};
}