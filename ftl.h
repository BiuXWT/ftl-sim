#ifndef FTL_H
#define FTL_H

#include <bits/stdc++.h>
#include "nand_model.h"
#include "nand_runtime.h"
#include "nand_driver.h"
#include "block_allocator.h"
using namespace std;

/* ---------------- FTL ---------------- */
class FTL
{
public:
    FTL(NandDriver &drv, NandRuntime &rt, BlockManager &alloc, int total_lbas);

    void write(int lba, const string &data);
    void read(int lba);

    void rebuild_from_oob();
    void dump_stats();
    void dump_page_stats();


private:
    NandDriver &nand_drive;
    NandRuntime &nand_runtime;
    BlockManager &block_manager;
    int total_pages_;
    uint64_t seq_;

    vector<int> L2P, P2L;
    vector<PageState> pstate;

    bool program_pba_with_handling(int &pba, const string &data, int lba);
    void erase_block_txn(int d, int p, int b /*PBN*/);
    void run_gc();

    // helpers
    int drv_vbn_to_pbn(int d, int p, int vbn);
    int pages_per_plane() const;
    int pages_per_die() const;
    int pba_from_indices(int d, int p, int b, int g) const;
    tuple<int, int, int, int> idx_from_pba(int pba) const;
};

#endif // FTL_H