#ifndef NAND_RUNTIME_H
#define NAND_RUNTIME_H

#include <bits/stdc++.h>
#include "nand_model.h"
using namespace std;

/* ---------------- NandRuntime (DRAM-side state) ---------------- */
struct NandRuntime
{
    int dies, planes, blocks;
    vector<uint32_t> erase_count; // per-block
    vector<uint32_t> prog_count;
    vector<bool> bad_block_table;                             // bad-block table
    unordered_set<uint64_t> injected_fail_blocks; // fault inject

    NandRuntime(int dies_, int planes_, int blocks_);
    void status();

    int idx(int d, int p, int b) const;
    uint64_t key(int d, int p, int b) const;
    bool should_fail(int d, int p, int b) const;

};

#endif // NAND_RUNTIME_H