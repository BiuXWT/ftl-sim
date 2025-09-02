#ifndef NAND_MODEL_H
#define NAND_MODEL_H

#include <bits/stdc++.h>
using namespace std;

/* ---------------- basic enums ---------------- */
enum class PageState
{
    EMPTY = 0,
    VALID = 1,
    INVALID = 2
};
enum class NandCmd
{
    READ_PAGE,
    PROGRAM_PAGE,
    ERASE_BLOCK
};

/* ---------------- NandModel (pure physical) ---------------- */
struct Page
{
    string data;
    int oob_lba = -1;
    uint64_t oob_seq = 0;
    uint8_t oob_bad = 0xFF; // 0xFF good, 0x00 bad (page0/page1)
};

struct Block
{
    vector<Page> pages;
    Block(int ppb);
};

struct Plane
{
    vector<Block> blocks;
    Plane(int ppb, int bpp);
};

struct Die
{
    vector<Plane> planes;
    Die(int ppb, int bpp, int ppd);
};

struct NandModel
{
    int pages_per_block, blocks_per_plane, planes_per_die, dies_per_nand;
    vector<Die> dies;
    NandModel(int dpn, int ppd, int bpp, int ppb);
    void dump_page_stats();
    void dump_page_data();

};

#endif // NAND_MODEL_H