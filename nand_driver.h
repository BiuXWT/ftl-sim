#ifndef NAND_DRIVER_H
#define NAND_DRIVER_H

#include <bits/stdc++.h>
#include "nand_model.h"
#include "nand_runtime.h"
using namespace std;

/* ---------------- NandOp / NandDriver ---------------- */
struct NandAddr
{
    int die, plane, block, page;
};

// NAND操作结果枚举
enum class NandStatus {
    SUCCESS = 0,
    FAILED = 1,
    BAD_BLOCK = 2,
    ECC_ERROR = 3,
    TIMEOUT = 4
};

struct NandOp
{
    NandCmd cmd;
    vector<NandAddr> targets;
    vector<string> data;      
    vector<int> oob_lba;      
    vector<uint64_t> oob_seq; 
};

// NAND驱动统计信息
struct NandStats {
    uint64_t read_ops = 0;
    uint64_t program_ops = 0;
    uint64_t erase_ops = 0;
    uint64_t failed_ops = 0;
    uint64_t bad_blocks_detected = 0;
};

class NandDriver
{
public:
    NandDriver(NandModel &model, NandRuntime &runtime);

    // control verbose logging from tests
    void set_verbose(bool v) { verbose_ = v; }

    // 提交NAND操作
    pair<NandStatus, string> submit(NandOp &op);
    
    // 检查块是否为坏块
    bool is_block_bad(int d, int p, int b) const;
    
    // 标记块为坏块
    void mark_block_bad_oob(int d, int p, int b);
    
    // NAND参数获取
    int pages_per_block() const;
    int blocks_per_plane() const;
    int planes_per_die() const;
    int dies_per_nand() const;

    // 获取块擦除计数
    uint32_t get_erase_count(int d, int p, int b) const;

    // 坏块注入（用于测试）
    void inject_factory_bad(int d, int p, int b);
    void inject_runtime_fail(int d, int p, int b);
    void clear_runtime_fail(int d, int p, int b);
    
    // 统计信息
    const NandStats& get_stats() const { return stats_; }
    void reset_stats() { stats_ = NandStats{}; }

private:
    NandModel &model_;
    NandRuntime &runtime_;
    NandStats stats_;
    // protect access to model_/runtime_/stats_ (use recursive to allow submit->read_page nesting)
    mutable std::mutex mtx_;
    bool verbose_ = false;

    bool valid_addr(const NandAddr &a) const;
    bool valid_block(int d, int p, int b) const;
    void erase_block(int d, int p, int b, bool preserve_bad_mark);
    
    // 内部操作执行方法
    pair<NandStatus, string> execute_read(NandOp &op);
    pair<NandStatus, string> execute_program(NandOp &op);
    pair<NandStatus, string> execute_erase(NandOp &op);
    // helpers
    pair<NandStatus,string> validate_op_common(const NandOp &op) const;
    pair<NandStatus,string> validate_targets_for_program(const NandOp &op) const;
};

#endif // NAND_DRIVER_H