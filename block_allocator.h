#ifndef BLOCK_ALLOCATOR_H
#define BLOCK_ALLOCATOR_H

#include <bits/stdc++.h>
#include "nand_model.h"
#include "nand_runtime.h"
#include "nand_driver.h"
using namespace std;

/* ---------------- BlockManager with BAD BLOCK TABLE ----------------
   - remap[d][p][vbn] = pbn (or -1 for identity)
   - reserved_write: normal reserved for writes
   - reserved_spare: only for BAD BLOCK TABLE
   - free list holds VBNs; open_block is VBN; pba组装时用 resolve_pbn()
   VBN:Virtual Block Number (0..blocks-1)
   PBN:Physical Block Number
*/
class BlockManager
{
public:
    struct PlaneManager
    {
        // VBN lists
        deque<int> free_vbns;
        deque<int> reserved_write_vbns;
        // spare pool (PBN indices) only for BAD BLOCK TABLE
        deque<int> reserved_spare_pbns;

        int open_vbn = -1;
        int next_page_on_open_pbn = 0;
    };

    BlockManager(NandDriver &drv, NandRuntime &rt, int reserved_write_per_plane, int reserved_spare_per_plane);

    // 初始化：构建 BAD BLOCK TABLE 前的列表，随后对 FACTORY BAD BLOCK 做 remap
    void init_from_bbt(function<bool(int, int, int)> is_bad_block);

    // 分配一个页（返回 PBA），VBN 由 allocator 维护
    int alloc_page(int die, int plane);
    
    // 分配一个块（返回VBN），用于GC等操作
    int alloc_block(int die, int plane);

    // GC/擦除完成后把该 **PBN** 所属的 VBN 送回 free（按身份或 remap 逆向）
    void on_erase_complete(int die, int plane, int pbn);

    // 如果 open_vbn 被涉及（比如它对应的 PBN 标坏），丢弃 open
    void drop_open_if_matches(int die, int plane, int pbn_or_vbn, bool input_is_pbn = true);

    // 运行时坏块替换：对 "坏的 PBN" 找到其 VBN 并 remap 到一个新的 spare PBN
    bool remap_grown_bad(int die, int plane, int bad_pbn);

    // 调试
    void dump_alloc_state();


    // —— 提供给上层的工具 —— //
    int resolve_pbn(int d, int p, int vbn) const;
    int pba_from_indices(int d, int p, int b, int g) const;
    tuple<int, int, int, int> idx_from_pba(int pba) const;

private:
    NandDriver &drv_;
    NandRuntime &nand_runtime;
    int reserved_write_; // per plane
    int reserved_spare_; // per plane (BAD BLOCK TABLE pool)
    vector<vector<PlaneManager>> plane_manager;
    // remap: [die][plane][vbn] -> pbn (or -1)
    vector<vector<vector<int>>> remap_;
    // 反向映射: [die][plane][pbn] -> vbn (用于O(1)查找)
    vector<vector<vector<int>>> reverse_remap_;

    bool valid_plane(int d, int p) const;

    // 使用页状态判断块是否空
    bool is_block_empty_by_state(int d, int p, int vbn,
                                 function<bool(int, int, int, int)> is_page_empty);

    // 从 spare pool 取一个 PBN
    int take_spare_pbn(int d, int p);

    // 反解：给 PBN 找到其 VBN（使用反向映射表）
    int reverse_resolve_vbn(int d, int p, int pbn) const;

    // wear-aware：在 VBN 列表里挑 erase_count 最小者（按当前 PBN）
    int pick_vbn_wear_aware(deque<int> &vbns, int d, int p);
    
    // 动态分配备用块
    bool dynamic_allocate_spare_block(int die, int plane);
};

#endif // BLOCK_ALLOCATOR_H