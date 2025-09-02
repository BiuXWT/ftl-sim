#include "nand_model.h"
#include <sys/stat.h>
#include <sys/types.h>

/* ---------------- NandModel (pure physical) ---------------- */
Block::Block(int ppb) : pages(ppb) {}

Plane::Plane(int ppb, int bpp)
{
    for (int i = 0; i < bpp; ++i)
        blocks.emplace_back(ppb);
}

Die::Die(int ppb, int bpp, int ppd)
{
    for (int i = 0; i < ppd; ++i)
        planes.emplace_back(ppb, bpp);
}

NandModel::NandModel(int dpn, int ppd, int bpp, int ppb)
    : pages_per_block(ppb), blocks_per_plane(bpp), planes_per_die(ppd), dies_per_nand(dpn)
{
    for (int d = 0; d < dpn; ++d)
        dies.emplace_back(ppb, bpp, ppd);
}
     
void NandModel::dump_page_stats()
{
    for (auto &die : dies){
        for (auto &plane : die.planes){
            for (auto &block : plane.blocks){
                for (auto &page : block.pages){
                    cout << (page.oob_bad == 0xFF ? "E" : "B")<< " ";
                }
                cout << endl;
            }
        }
    }
}
void NandModel::dump_page_data()
{
    for (auto &die : dies){
        for (auto &plane : die.planes){
            for (auto &block : plane.blocks){
                if(block.pages[0].oob_bad==0x00){
                    cout << setw(6) << "BAD" << endl;
                    continue;
                }
                for (auto &page : block.pages){
                    cout << setw(6) << page.data<< " ";
                }
                cout << endl;
            }
        }
    }
}