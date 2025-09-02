#include "nand_runtime.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <fstream>
#include <iomanip>

NandRuntime::NandRuntime(int dies_, int planes_, int blocks_)
    : dies(dies_), planes(planes_), blocks(blocks_),
      erase_count(dies_ * planes_ * blocks_, 0),
      prog_count(dies_ * planes_ * blocks_, 0),
      bad_block_table(dies_ * planes_ * blocks_, false) {}

void NandRuntime::status()
{
    cout << "================== NandRuntime Status ==================\n";
    cout << "Dies: " << dies << ", Planes: " << planes << ", Blocks: " << blocks << "\n";
    cout << "Erase Count:\t";
    for (const auto& count : erase_count) {
        cout << setw(6) << count << " ";
    }
    cout << "\nProg Count:\t";
    for (const auto& count : prog_count) {
        cout << setw(6) << count << " ";
    }
    cout << "\nBadBlockTable:\t";
    for (const auto& is_bad : bad_block_table) {
        cout << setw(6) << (is_bad ? 1 : 0) << " ";
    }
    cout << "\n=========================================================\n";
}

int NandRuntime::idx(int d, int p, int b) const { 
    return ((d * planes) + p) * blocks + b; 
}
uint64_t NandRuntime::key(int d, int p, int b) const { return ((uint64_t)d << 30) | ((uint64_t)p << 20) | (uint64_t)b; }
bool NandRuntime::should_fail(int d, int p, int b) const { return injected_fail_blocks.count(key(d, p, b)) > 0; }
