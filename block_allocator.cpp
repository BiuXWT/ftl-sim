#include "block_allocator.h"
#include <sys/stat.h>
#include <sys/types.h>

/* ---------------- BlockManager with BAD BLOCK TABLE ---------------- */
BlockManager::BlockManager(NandDriver &drv, NandRuntime &rt, int reserved_write_per_plane, int reserved_spare_per_plane)
    : drv_(drv), nand_runtime(rt), reserved_write_(reserved_write_per_plane), reserved_spare_(reserved_spare_per_plane)
{
    plane_manager.resize(drv_.dies_per_nand(), vector<PlaneManager>(drv_.planes_per_die()));
    remap_.resize(drv_.dies_per_nand(), vector<vector<int>>(drv_.planes_per_die(), vector<int>(drv_.blocks_per_plane(), -1)));
    // 初始化反向映射表
    reverse_remap_.resize(drv_.dies_per_nand(), vector<vector<int>>(drv_.planes_per_die(), vector<int>(drv_.blocks_per_plane(), -1)));
    // 初始化时，所有PBN都映射到相同VBN（身份映射）
    for (int d = 0; d < drv_.dies_per_nand(); ++d) {
        for (int p = 0; p < drv_.planes_per_die(); ++p) {
            for (int b = 0; b < drv_.blocks_per_plane(); ++b) {
                reverse_remap_[d][p][b] = b;
            }
        }
    }
}

// 初始化：构建 BAD BLOCK TABLE 前的列表，随后对 FACTORY BAD BLOCK 做 remap
void BlockManager::init_from_bbt(function<bool(int, int, int)> is_bad_block)
{
    for (int d = 0; d < drv_.dies_per_nand(); ++d)
    {
        for (int p = 0; p < drv_.planes_per_die(); ++p)
        {
            auto &pl = plane_manager[d][p];
            pl.free_vbns.clear();
            pl.reserved_write_vbns.clear();
            pl.reserved_spare_pbns.clear();

            // 末尾划分： [.. normal .. | reserved_write | reserved_spare ]
            int total = drv_.blocks_per_plane();
            int reserved_spare = max(0, reserved_spare_);
            int reserved_write = max(0, reserved_write_);
            if (reserved_spare + reserved_write > total)
            {
                reserved_spare = min(reserved_spare, total);
                reserved_write = max(0, total - reserved_spare);
            }

            int start_write = total - (reserved_write + reserved_spare);
            int start_spare = total - reserved_spare;

            // reserved_spare 用 PBN 列表存（因为仅做替换，不直接作为 VBN 分配）
            for (int b = start_spare; b < total; ++b)
            {
                if (!is_bad_block(d, p, b))
                    pl.reserved_spare_pbns.push_back(b);
            }
            // reserved_write 和 normal 作为 VBN 列表存
            for (int b = start_write; b < start_spare; ++b)
            {
                if (!is_bad_block(d, p, b))
                    pl.reserved_write_vbns.push_back(b);
            }
            for (int b = 0; b < start_write; ++b)
            {
                if (!is_bad_block(d, p, b))
                    pl.free_vbns.push_back(b);
            }

            // 工厂坏块：对每个 FACTORY BAD BLOCK vbn 进行 remap（占用一个 spare_pbn）
            for (int vbn = 0; vbn < drv_.blocks_per_plane(); ++vbn)
            {
                if (is_bad_block(d, p, vbn))
                {
                    int spare = take_spare_pbn(d, p);
                    if (spare != -1)
                    {
                        remap_[d][p][vbn] = spare; // VBN -> spare PBN
                        reverse_remap_[d][p][spare] = vbn; // 更新反向映射
                        // FACTORY BAD BLOCK 的 VBN 也应该可用（映射到 spare），加入 free_vbns
                        // 注意避免把它放到 reserved 区（让它进入 normal free 更简单）
                        pl.free_vbns.push_back(vbn);
                    }
                    else
                    {
                        // 没有 spare，VBN 彻底不可用：不入 free/reserved（容量下降）
                    }
                }
            }

            pl.open_vbn = !pl.free_vbns.empty() ? pl.free_vbns.front() : -1;
            if(pl.open_vbn == pl.free_vbns.front()) pl.free_vbns.pop_front();
            pl.next_page_on_open_pbn = 0;
        }
    }
}

// 分配一个页（返回 PBA），VBN 由 allocator 维护
int BlockManager::alloc_page(int die, int plane)
{
    if (!valid_plane(die, plane))
        return -1;
    auto &pl = plane_manager[die][plane];

    // 确保 open_vbn 存在且 PBN 还有页可写
    if (pl.open_vbn == -1 || pl.next_page_on_open_pbn >= drv_.pages_per_block())
    {
        // 先从 free_vbns 取一个 VBN（wear-aware）
        int v = pick_vbn_wear_aware(pl.free_vbns, die, plane);
        if (v == -1)
        {
            // 再从 reserved_write_vbns 取
            v = pick_vbn_wear_aware(pl.reserved_write_vbns, die, plane);
        }
        if (v == -1)
            return -1;

        pl.open_vbn = v;
        pl.next_page_on_open_pbn = 0;
    }

    int pbn = resolve_pbn(die, plane, pl.open_vbn);
    int page = pl.next_page_on_open_pbn++;
    return pba_from_indices(die, plane, pbn, page);
}

// 分配一个块（返回VBN），用于GC等操作
int BlockManager::alloc_block(int die, int plane)
{
    if (!valid_plane(die, plane))
        return -1;
        
    auto &pl = plane_manager[die][plane];
    
    // 从 free_vbns 取一个 VBN（wear-aware）
    int v = pick_vbn_wear_aware(pl.free_vbns, die, plane);
    if (v == -1)
    {
        // 再从 reserved_write_vbns 取
        v = pick_vbn_wear_aware(pl.reserved_write_vbns, die, plane);
    }
    if (v == -1)
        return -1;
        
    // 不要将该VBN设置为open_vbn，因为这是专门用于GC的块分配
    return v;
}

// GC/擦除完成后把该 **PBN** 所属的 VBN 送回 free（按身份或 remap 逆向）
void BlockManager::on_erase_complete(int die, int plane, int pbn)
{
    int vbn = reverse_resolve_vbn(die, plane, pbn);
    if (vbn < 0)
        return;
    plane_manager[die][plane].free_vbns.push_back(vbn);
}

// 如果 open_vbn 被涉及（比如它对应的 PBN 标坏），丢弃 open
void BlockManager::drop_open_if_matches(int die, int plane, int pbn_or_vbn, bool input_is_pbn)
{
    auto &pl = plane_manager[die][plane];
    if (pl.open_vbn == -1)
        return;
    int cur_pbn = resolve_pbn(die, plane, pl.open_vbn);
    int x = input_is_pbn ? pbn_or_vbn : resolve_pbn(die, plane, pbn_or_vbn);
    if (cur_pbn == x)
    {
        pl.open_vbn = -1;
        pl.next_page_on_open_pbn = 0;
    }
}

// 运行时坏块替换：对 "坏的 PBN" 找到其 VBN 并 remap 到一个新的 spare PBN
bool BlockManager::remap_grown_bad(int die, int plane, int bad_pbn)
{
    int vbn = reverse_resolve_vbn(die, plane, bad_pbn);
    if (vbn < 0)
        return false;
    // 取一个新的 spare PBN
    int spare = take_spare_pbn(die, plane);
    // 如果没有备用块，尝试动态分配备用块
    if (spare == -1) {
        if (!dynamic_allocate_spare_block(die, plane)) {
            return false;
        }
        spare = take_spare_pbn(die, plane);
        if (spare == -1) {
            return false;
        }
    }
    remap_[die][plane][vbn] = spare;
    reverse_remap_[die][plane][spare] = vbn; // 更新反向映射
    reverse_remap_[die][plane][bad_pbn] = -1; // 清除旧的反向映射
    // 如果 open 指向该 VBN，丢弃（由上层重新分配）
    drop_open_if_matches(die, plane, vbn, /*input_is_pbn=*/false);
    return true;
}

// 调试
void BlockManager::dump_alloc_state()
{
    for (int d = 0; d < drv_.dies_per_nand(); ++d)
    {
        for (int p = 0; p < drv_.planes_per_die(); ++p)
        {
            auto &pl = plane_manager[d][p];
            cout << "[ALLOC] die" << d << "/plane" << p
                 << " open_vbn=" << pl.open_vbn << " nextp=" << pl.next_page_on_open_pbn
                 << " freeV=" << pl.free_vbns.size()
                 << " resW=" << pl.reserved_write_vbns.size()
                 << " resS=" << pl.reserved_spare_pbns.size()
                 << "\n";
        }
    }
}

// —— 提供给上层的工具 —— //
int BlockManager::resolve_pbn(int d, int p, int vbn) const
{
    int r = remap_[d][p][vbn];
    return (r >= 0 ? r : vbn);
}

int BlockManager::pba_from_indices(int d, int p, int b, int g) const
{
    int pages_per_plane = drv_.pages_per_block() * drv_.blocks_per_plane();
    int pages_per_die = pages_per_plane * drv_.planes_per_die();
    return d * pages_per_die + p * pages_per_plane + b * drv_.pages_per_block() + g;
}

tuple<int, int, int, int> BlockManager::idx_from_pba(int pba) const
{
    int ppb = drv_.pages_per_block(), bpp = drv_.blocks_per_plane(), ppd = drv_.planes_per_die();
    int pplane = ppb * bpp, pdie = pplane * ppd;
    int d = pba / pdie;
    int p = (pba % pdie) / pplane;
    int b = (pba % pplane) / ppb;
    int g = pba % ppb;
    return {d, p, b, g};
}

bool BlockManager::valid_plane(int d, int p) const { return d >= 0 && d < drv_.dies_per_nand() && p >= 0 && p < drv_.planes_per_die(); }

// 使用页状态判断块是否空
bool BlockManager::is_block_empty_by_state(int d, int p, int vbn,
                             function<bool(int, int, int, int)> is_page_empty)
{
    int pbn = resolve_pbn(d, p, vbn);
    for (int g = 0; g < drv_.pages_per_block(); ++g)
        if (!is_page_empty(d, p, pbn, g))
            return false;
    return true;
}

// 从 spare pool 取一个 PBN
int BlockManager::take_spare_pbn(int d, int p)
{
    auto &pl = plane_manager[d][p];
    if (pl.reserved_spare_pbns.empty())
        return -1;
    int b = pl.reserved_spare_pbns.front();
    pl.reserved_spare_pbns.pop_front();
    return b;
}

// 反解：给 PBN 找到其 VBN（使用反向映射表）
int BlockManager::reverse_resolve_vbn(int d, int p, int pbn) const
{
    // 使用反向映射表直接查找
    if (d < 0 || d >= (int)reverse_remap_.size() || 
        p < 0 || p >= (int)reverse_remap_[d].size() || 
        pbn < 0 || pbn >= (int)reverse_remap_[d][p].size()) {
        return -1;
    }
    return reverse_remap_[d][p][pbn];
}

// wear-aware：在 VBN 列表里挑 erase_count 最小者（按当前 PBN）
int BlockManager::pick_vbn_wear_aware(deque<int> &vbns, int d, int p)
{
    if (vbns.empty())
        return -1;
    int best_pos = -1, best_v = -1;
    uint32_t best_ec = UINT32_MAX;
    for (int i = 0; i < (int)vbns.size(); ++i)
    {
        int v = vbns[i];
        int phys = resolve_pbn(d, p, v);
        if (nand_runtime.bad_block_table[nand_runtime.idx(d, p, phys)])
            continue; // 保险
        uint32_t ec = nand_runtime.erase_count[nand_runtime.idx(d, p, phys)];
        if (ec < best_ec)
        {
            best_ec = ec;
            best_pos = i;
            best_v = v;
        }
    }
    if (best_pos == -1)
        return -1;
    vbns.erase(vbns.begin() + best_pos);
    return best_v;
}

// 动态分配备用块
bool BlockManager::dynamic_allocate_spare_block(int die, int plane)
{
    if (!valid_plane(die, plane))
        return false;
    
    auto& pl = plane_manager[die][plane];
    
    // 如果没有空闲块，无法分配备用块
    if (pl.free_vbns.empty() && pl.reserved_write_vbns.empty())
        return false;
    
    // 优先从free_vbns中分配
    int vbn = -1;
    if (!pl.free_vbns.empty()) {
        // 选择擦除次数最少的块
        vbn = pick_vbn_wear_aware(pl.free_vbns, die, plane);
    } else if (!pl.reserved_write_vbns.empty()) {
        // 如果free_vbns为空，则从reserved_write_vbns中分配
        vbn = pick_vbn_wear_aware(pl.reserved_write_vbns, die, plane);
    }
    
    if (vbn == -1)
        return false;
    
    // 将选中的VBN对应的物理块添加到备用池
    int pbn = resolve_pbn(die, plane, vbn);
    pl.reserved_spare_pbns.push_back(pbn);
    
    return true;
}