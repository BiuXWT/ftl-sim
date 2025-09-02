#include "ftl.h"
#include <iostream>
void  page_stats(FTL& ftl,NandModel& model)
{
    std::cout << "================== ftl Page Stats =================\n";
    ftl.dump_page_stats();

    std::cout << "================== model Page Stats ==================\n";

    model.dump_page_stats();
    std::cout << "================== end Page Stats =================\n";
}
void runtime_stat(FTL &ftl,BlockManager &block_manager)
{
    ftl.dump_stats();
    block_manager.dump_alloc_state();
}

int main()
{
    int dies_per_nand = 1;
    int planes_per_die = 1;
    int blocks_per_plane = 8;
    int pages_per_block = 8;
    int reserved_write_blocks_per_plane = 1;
    int reserved_spare_blocks_per_plane = 2;

    int total_pages = pages_per_block * blocks_per_plane * planes_per_die * dies_per_nand;
    int total_lbas = total_pages - pages_per_block * (reserved_write_blocks_per_plane + reserved_spare_blocks_per_plane) * planes_per_die * dies_per_nand;

    cout << "total_lbas: " << total_lbas << endl;
    cout << "total_pages: " << total_pages << endl;

    NandModel model(dies_per_nand, planes_per_die, blocks_per_plane, pages_per_block);
    NandRuntime runtime(dies_per_nand, planes_per_die, blocks_per_plane);
    NandDriver driver(model, runtime);
    driver.inject_factory_bad(0, 0, 1);

    BlockManager block_manager(driver, runtime, reserved_write_blocks_per_plane, reserved_spare_blocks_per_plane);
    FTL ftl(driver, runtime, block_manager, total_lbas);

    // 连续写，期间注入一个运行时坏块（例如 PBN=3）
    for (int i = 0; i < 16 ; ++i){
        ftl.write(i % total_lbas, "D" + to_string(i));
    }
    cout << "-- before GROWN BAD BLOCK --\n";
    runtime_stat(ftl, block_manager);
    page_stats(ftl, model);

    driver.inject_runtime_fail(0, 0, 3);//注入坏块 PBN=3
    for (int i = 16; i < total_lbas; ++i)
        ftl.write(i % total_lbas, "D" + to_string(i));
    cout << "-- after GROWN BAD BLOCK & BAD BLOCK TABLE --\n";
    runtime_stat(ftl, block_manager);

    page_stats(ftl, model);



    // 触发GC
    for (int i = total_lbas ; i < total_lbas*100; ++i)
        ftl.write(i % total_lbas, "D" + to_string(i));
    cout << "-- after GC --\n";
    runtime_stat(ftl, block_manager);

    cout << "-- ftl page data --\n";
    for (int lba = 0; lba < total_lbas; ++lba)
    {
        ftl.read(lba);
        if(lba%pages_per_block==(pages_per_block-1)){
            cout << endl;
        }
    }
    cout << "-- nand page data --\n";
    model.dump_page_data();
    page_stats(ftl, model);


    runtime_stat(ftl, block_manager);

    runtime.status();

    return 0;
}