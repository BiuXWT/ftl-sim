
# FTL-SIM

本项目是一个用于 NAND 闪存的块分配与坏块管理的模拟系统，适用于 SSD FTL（Flash Translation Layer）等场景。支持多 plane、多 die、坏块表、动态坏块替换、GC、磨损均衡等完整的块管理流程。

---

## 目录结构与核心模块

- `nand_model`：
	- 定义 NAND 闪存的基本结构（如 die、plane、block、page），模拟物理特性。
- `nand_driver`：
	- 提供对 NAND 模型的操作接口，包括读写擦除等。
- `nand_runtime`：
	- 记录运行时状态，如块擦除计数、坏块信息等。
- `block_allocator`：
	- 定义BlockManager，管理空闲块、备用块池、坏块，负责虚拟块（VBN）到物理块（PBN）的映射、GC 回收、动态坏块 remap、磨损均衡等。
	- 支持 remap 表和反向 remap，便于坏块替换和调试。
- `ftl`：
	- Flash Translation Layer 层，负责L2P和P2L映射，调用 BlockManager 进行块分配。
- `main`：
	- 程序入口，包含测试用例或仿真流程。
- `build.sh`：
	- 一键构建脚本。
- `CMakeLists.txt`：
	- CMake 构建配置。

---

## 编译与运行

### 1. 使用 CMake 构建

```bash
mkdir build-release
cd build-release
cmake ..
make
```

### 2. 使用脚本构建

```bash
./build.sh
```

### 3. 运行

编译后会生成 `ftl` 可执行文件：

```bash
./build-release/ftl
```
---

## 许可证
MIT License
