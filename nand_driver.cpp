#include "nand_driver.h"
#include <mutex>

/* ---------------- NandDriver ---------------- */
NandDriver::NandDriver(NandModel &model, NandRuntime &runtime)
    : model_(model), runtime_(runtime) {}

pair<NandStatus, string> NandDriver::submit(NandOp &op)
{
    // make submit thread-safe and validate basic constraints
    std::lock_guard<std::mutex> lk(mtx_);
    auto v = validate_op_common(op);
    if (v.first != NandStatus::SUCCESS) {
        stats_.failed_ops++;
        return v;
    }
    switch (op.cmd) {
        case NandCmd::READ_PAGE:
            stats_.read_ops++;
            return execute_read(op);
            
        case NandCmd::PROGRAM_PAGE:
            stats_.program_ops++;
            return execute_program(op);
            
        case NandCmd::ERASE_BLOCK:
            stats_.erase_ops++;
            return execute_erase(op);
            
        default:
            stats_.failed_ops++;
            return {NandStatus::FAILED, "unknown command"};
    }
}

pair<NandStatus, string> NandDriver::execute_read(NandOp &op)
{
    op.data.clear(); op.oob_lba.clear(); op.oob_seq.clear();
    for (const auto &a : op.targets) {
        //检查是否是注入的坏块
        if (runtime_.should_fail(a.die, a.plane, a.block)) {
            stats_.failed_ops++;
            return {NandStatus::FAILED, "injected failure"};
        }
        if (is_block_bad(a.die, a.plane, a.block)) {
            stats_.bad_blocks_detected++;
            return {NandStatus::BAD_BLOCK, "bad block"};
        }
        const Page pg = model_.dies[a.die].planes[a.plane].blocks[a.block].pages[a.page];
        op.data.push_back(pg.data);
        op.oob_lba.push_back(pg.oob_lba);
        op.oob_seq.push_back(pg.oob_seq);
    }
    return {NandStatus::SUCCESS, "read success"};
}

pair<NandStatus, string> NandDriver::execute_program(NandOp &op)
{
    // parameter consistency validated in submit
    for (size_t i = 0; i < op.targets.size(); ++i) {
        const auto &a = op.targets[i];
        if (runtime_.should_fail(a.die, a.plane, a.block)) {
            stats_.failed_ops++;
            return {NandStatus::FAILED, "injected failure"};
        }
        if (is_block_bad(a.die, a.plane, a.block)) {
            stats_.bad_blocks_detected++;
            return {NandStatus::BAD_BLOCK, "bad block"};
        }
        Page &pg = model_.dies[a.die].planes[a.plane].blocks[a.block].pages[a.page];
        if (!(pg.data.empty() && pg.oob_seq == 0)) {
            stats_.failed_ops++;
            return {NandStatus::FAILED, "program on non-erased page"};
        }
        if (!op.data.empty()) pg.data = op.data[i];
        if (!op.oob_lba.empty()) pg.oob_lba = op.oob_lba[i];
        if (!op.oob_seq.empty()) pg.oob_seq = op.oob_seq[i];
        if (verbose_) std::cout << "pba[" << a.die << ":" << a.plane << ":" << a.block << ":" << a.page << "] data:" << pg.data << " lba" << pg.oob_lba << std::endl;
        runtime_.prog_count[runtime_.idx(a.die, a.plane, a.block)]++;
    }
    return {NandStatus::SUCCESS, "program success"};
}

pair<NandStatus, string> NandDriver::execute_erase(NandOp &op)
{
    for (const auto &a : op.targets) {
        if (!valid_block(a.die, a.plane, a.block)) {
            stats_.failed_ops++;
            return {NandStatus::FAILED, "invalid block"};
        }
        if (runtime_.should_fail(a.die, a.plane, a.block)) {
            stats_.failed_ops++;
            return {NandStatus::FAILED, "injected failure"};
        }
        if (is_block_bad(a.die, a.plane, a.block)) {
            stats_.bad_blocks_detected++;
            return {NandStatus::BAD_BLOCK, "bad block"};
        }
        erase_block(a.die, a.plane, a.block, true);
    }
    return {NandStatus::SUCCESS, "erase success"};
}

pair<NandStatus,string> NandDriver::validate_op_common(const NandOp &op) const
{
    if (op.targets.empty()) return {NandStatus::FAILED, "no targets"};
    // basic checks for target addresses
    for (const auto &a : op.targets) {
        if (!valid_block(a.die, a.plane, a.block)) return {NandStatus::FAILED, "invalid block"};
        if (op.cmd == NandCmd::READ_PAGE) {
            if (a.page < 0 || a.page >= model_.pages_per_block) return {NandStatus::FAILED, "invalid page"};
        }
    }
    // PROGRAM specific param counts
    if (op.cmd == NandCmd::PROGRAM_PAGE) {
        if (!op.data.empty() && op.data.size() != op.targets.size()) return {NandStatus::FAILED, "data size mismatch"};
        if (!op.oob_lba.empty() && op.oob_lba.size() != op.targets.size()) return {NandStatus::FAILED, "oob_lba size mismatch"};
        if (!op.oob_seq.empty() && op.oob_seq.size() != op.targets.size()) return {NandStatus::FAILED, "oob_seq size mismatch"};
    }
    return {NandStatus::SUCCESS, "ok"};
}

pair<NandStatus,string> NandDriver::validate_targets_for_program(const NandOp &op) const
{
    for (size_t i = 0; i < op.targets.size(); ++i) {
        auto a = op.targets[i];
        // ensure page within range
        if (a.page < 0 || a.page >= model_.pages_per_block) return {NandStatus::FAILED, "invalid page"};
        // check bad block or runtime fail
        if (runtime_.should_fail(a.die, a.plane, a.block)) return {NandStatus::FAILED, "injected failure"};
        if (is_block_bad(a.die, a.plane, a.block)) return {NandStatus::BAD_BLOCK, "bad block"};
        const Page &pg = model_.dies[a.die].planes[a.plane].blocks[a.block].pages[a.page];
        if (!(pg.data.empty() && pg.oob_seq == 0)) return {NandStatus::FAILED, "program on non-erased page"};
    }
    return {NandStatus::SUCCESS, "ok"};
}

bool NandDriver::is_block_bad(int d, int p, int b) const
{
    if (!valid_block(d, p, b))
        return true;
    const auto &blk = model_.dies[d].planes[p].blocks[b];
    uint8_t b0 = blk.pages[0].oob_bad;
    uint8_t b1 = (model_.pages_per_block >= 2) ? blk.pages[1].oob_bad : 0xFF;
    return (b0 != 0xFF) || (b1 != 0xFF);
}

void NandDriver::mark_block_bad_oob(int d, int p, int b)
{
    if (!valid_block(d, p, b))
        return;
    auto &blk = model_.dies[d].planes[p].blocks[b];
    if (model_.pages_per_block >= 1)
        blk.pages[0].oob_bad = 0x00;
    if (model_.pages_per_block >= 2)
        blk.pages[1].oob_bad = 0x00;
    
    stats_.bad_blocks_detected++;
    std::cout << "Marking block [" << d << "-" << p << "-" << b << "] bad" << std::endl;
}

int NandDriver::pages_per_block() const 
{ 
    return model_.pages_per_block; 
}

int NandDriver::blocks_per_plane() const 
{ 
    return model_.blocks_per_plane; 
}

int NandDriver::planes_per_die() const 
{ 
    return model_.planes_per_die; 
}

int NandDriver::dies_per_nand() const 
{ 
    return model_.dies_per_nand; 
}

uint32_t NandDriver::get_erase_count(int d, int p, int b) const 
{ 
    return runtime_.erase_count[runtime_.idx(d, p, b)]; 
}

void NandDriver::inject_factory_bad(int d, int p, int b) 
{ 
    mark_block_bad_oob(d, p, b); 
}

void NandDriver::inject_runtime_fail(int d, int p, int b) 
{ 
    runtime_.injected_fail_blocks.insert(runtime_.key(d, p, b)); 
}

void NandDriver::clear_runtime_fail(int d, int p, int b) 
{ 
    runtime_.injected_fail_blocks.erase(runtime_.key(d, p, b)); 
}

bool NandDriver::valid_addr(const NandAddr &a) const
{
    return valid_block(a.die, a.plane, a.block) && a.page >= 0 && a.page < model_.pages_per_block;
}

bool NandDriver::valid_block(int d, int p, int b) const
{
    return d >= 0 && d < model_.dies_per_nand && p >= 0 && p < model_.planes_per_die && b >= 0 && b < model_.blocks_per_plane;
}

void NandDriver::erase_block(int d, int p, int b, bool preserve_bad_mark)
{
    if (!valid_block(d, p, b))
        return;
    Block &blk = model_.dies[d].planes[p].blocks[b];
    for (int pg = 0; pg < model_.pages_per_block; ++pg)
    {
        Page &pgi = blk.pages[pg];
        pgi.data.clear();
        pgi.oob_lba = -1;
        pgi.oob_seq = 0;
        if (!preserve_bad_mark)
            pgi.oob_bad = 0xFF;
    }
    runtime_.erase_count[runtime_.idx(d, p, b)]++;
}