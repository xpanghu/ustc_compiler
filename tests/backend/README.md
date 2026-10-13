# LoongArch Machine IR 后端

M1、M2 与 Lab6 线性扫描已实现。M1 提供保守指令选择、Machine IR、PHI/ABI lowering、全栈分配、frame lowering 和汇编输出；M2 增加三组局部贪心规则。默认后端仍为 legacy，MIR 后端默认使用 baseline 和 stack；linear 必须显式选择。

## 使用

从仓库根目录运行：

```sh
# M1：保守选择，可配合已有 LightIR 优化
build/cminusfc -S -mem2reg -backend=mir -o example.s example.cminus

# M2 + Lab6：贪心选择、线性扫描，保存阶段快照与分配报告
build/cminusfc -S -mem2reg -backend=mir -isel=greedy -regalloc=linear -dump-mir \
  -o example.s example.cminus
```

`-dump-mir` 生成 `example.selected.mir`、`example.prera.mir`、`example.allocated.mir`、`example.final.mir`；linear 额外生成 `example.regalloc.txt`；汇编路径还保留现有 `.ll` sidecar。`-backend` 只能用于 `-S`，`-isel`、`-regalloc`、`-dump-mir`、`-lab4-metrics` 只能用于 MIR 后端。未知选项值、模式冲突及不支持的 ABI 在创建后端输出前报错。

MIR 默认关闭 Lab4 插桩；显式加 `-lab4-metrics` 可生成实际度量调用。此时保留事件顺序、权重和模块 alloca 字节数，禁用比较链及地址链等多节点融合，仍允许单节点立即数选择。

## 阶段与支持范围

```text
LightIR
  → select → Selected snapshot
  → PHI、ABI、metrics lowering → PreRA snapshot
  → stack 或 linear allocation → Allocated snapshot
  → frame lowering
  → Final snapshot → assembly printer
```

- 值类型：I1、I32、Ptr64、F32。I1 保持 0/1，用户内存占 1 字节、spill 占 4 字节；I32 运算保持低 32 位的符号扩展表示；浮点常量按位保存。
- 覆盖当前 Cminus 的算术、六种整数及浮点比较、I1→I32 的 zext、I32↔F32 数值转换、入口块中的固定大小 alloca、全局及数组初始化、GEP、load/store、分支、PHI、直接调用及返回。非入口块 alloca 在选择阶段明确拒绝，避免将重复执行的分配错误地复用为同一个栈槽。
- ABI 分别支持最多 8 个整数/指针参数和 8 个 float 参数，支持 void 或单个标量返回。栈参数、可变参数、aggregate、间接调用、动态 alloca 不在本次范围；浮点参数不向整数参数寄存器溢出。
- PHI 在机器层按并行拷贝语义消除；多后继前驱使用对应边块，循环交换和三值环通过显式临时值解决，输入 LightIR 不被改写。
- 全栈分配为实际保留的 VReg 分配 home；普通指令先 reload 所有 use，再执行并 spill def，读写操作数同时建模。保留 `$t0..$t2`、`$ft0..$ft2`；`$t8` 专用于地址和大偏移展开。
- 栈帧按 16 字节对齐，先分配空间再保存 ra/fp；大帧、FrameIndex、常量及 COPY 均在 Final 之前展开。printer 不隐藏 spill 或大偏移处理。`LA_LOCAL` 保留为目标汇编器伪指令。
- 后端不再提供独立 MIR / 分配结果 verifier，也不再承诺诊断任意手工构造的非法 MIR；保留生成阶段的局部错误处理。分支距离不再由编译器提前检查，长分支 relaxation 仍不支持。

贪心规则先逆序规划消费者，再按原顺序输出。完整匹配成功后才提交 covered 标记，只吸收同块、单用途的纯节点，遇到 load/store/call 屏障或共享值时回退：

1. I32 add/sub 使用 signed12 `ADDI_W`，减法在 int64 中取负，避免 INT_MIN 溢出。
2. 六种有符号整数比较直接生成条件分支；移除纯 zext/ne0 条件链。对于浮点比较、I1 参数或 I1 load，只消除条件链并保留原生产者。
3. 按类型 stride 计算全常量 GEP 偏移，signed12 范围内折叠进 load/store，否则保留地址计算。

## 构建与测试

```sh
cmake --build build --target cminusfc test_machine_codegen test_register_allocation -j6
build/test_machine_codegen all
build/test_register_allocation all
```

可单独运行 `representation`、`baseline`、`alloca`、`phi`、`calls`、`metrics`、`stack`、`frame`、`printer`、`immediates`、`branches`、`addresses` 分组。`alloca` 构造循环分配并保留前次指针，检查 baseline/greedy 及 metrics 开/关均拒绝该输入且诊断包含函数和块。`emit-interop DIRECTORY` 另行生成供 C 互调测试使用的 LLVM 和 baseline/greedy × stack/linear 四种 MIR 汇编。

已移除专门构造非法 MIR / 分配结果的 verifier 测试与 `verifier`、`verify` 分组；保留算法结果断言、scratch 冲突负例、LLVM verifier 和目标执行测试。支配性、路径定义、寄存器区间冲突不再由独立后端校验器检查。

本次交叉工具链位于 `jianmu-dev` 容器：仓库挂载到 `/workspace`，`/workspace/build` 是独立构建卷，不能与 macOS 的同名目录二进制混用。

```sh
docker exec jianmu-dev cmake --build /workspace/build \
  --target cminusfc test_machine_codegen test_register_allocation -j6
docker exec jianmu-dev /workspace/build/test_machine_codegen all
docker exec jianmu-dev python3 /workspace/tests/backend/test_backend.py \
  --compiler /workspace/build/cminusfc --clang clang --opt opt \
  --cross-cc loongarch64-unknown-linux-gnu-gcc --qemu qemu-loongarch64 \
  --report /tmp/mir-validation.json
```

runner 的 `--mode` 支持 `raw`、`mem2reg`（默认）、`const-prop`、`gvn`、`licm`、`all`。后三个单 pass 模式先运行 mem2reg；`all` 为 mem2reg + const-prop + GVN + LICM。`--regalloc stack|linear|both` 默认 both；普通程序默认为 native LLVM / legacy / baseline_stack / baseline_linear / greedy_stack / greedy_linear 六路。`--baseline-only` 只过滤选择器，保留选中的分配器；例如默认 both 时为四路，配合 stack 时为三路。`--cli-only` 不需要目标工具链。

`--course lab3|lab4|lab6-functional|lab6-performance` 检查 `.out` 末行退出状态，前面字节为 stdout；仅归一化 CRLF→LF，保留空格和其余输出。目录为空、缺 golden、工具缺失、编译/链接/参考/目标执行失败均整体失败，成功报告仅记录完成的路径。`--timeout` 默认 60 秒、须大于 0，传到每个构建与执行子进程；Lab6 性能课程显式使用 `--timeout 300`。

普通入口自动执行完整 64 位 ABI oracle（3 次执行，检查 17 个实际 callee-saved body 定义，含 clobber/truncation 两个负例）与压力 oracle（both 时 6 次目标执行、24 个已知值子例，寄存器池 0/1/2）。压力执行遵守 regalloc 过滤；ABI oracle 固定检查 linear，独立于选择器和 regalloc 过滤。CLI 合法性检查也保持完整矩阵。

C 互调是 native LLVM + 选中 MIR 配置（默认 5 路），不包含 legacy；metrics 为 legacy + 选中 MIR 配置（默认 5 条目标路径），native LLVM 没有旧 CodeGen 的 metrics 插桩。metrics 固定 mem2reg。JSON 保存各入口实际完成的列表或计数，不将这些入口称为六路。

LLVM 参考使用 host clang 和源码 `src/io/io.c`；目标路径由交叉 GCC 静态链接同一源码后经 QEMU 执行。测试侧为 `void main` 包装返回 0 的 LLVM 入口。C 调用 MIR 的导出函数测试使用 raw 模式，因为现有全程序 DCE 以 main 为根，会删除无 main 翻译单元中的导出函数。

## 验证记录（2026-10-13）

实际工具：macOS ASAN/UBSAN 构建使用 Homebrew clang++ 22.1.8（CMake cache），opt 22.1.8、Python 3.14.6；shell 默认 clang 为 Apple 17.0.0，未用于本次 Docker LLVM 参考。容器 clang/opt 14.0.0、LoongArch GCC 12.0.1 20220210 experimental、QEMU 6.2.50。宿主 `build` 与容器 `/workspace/build` 分别构建。

| 验证 | 本次完成结果 |
|---|---|
| 内部回归 | macOS ASAN/UBSAN 与 Docker 的 register-allocation all、machine-codegen all 均退出 0 |
| CLI / 模式单元 | 两种环境 CLI 均通过 19 个拒绝输入；模式单元各 3/3 |
| 普通六路 | raw、mem2reg、all 各 10 程序 / 17 输入 / 102 次执行；合计 306 次 |
| Lab3 mem2reg | 14 程序、84 次执行全部匹配 golden |
| Lab4 LICM | 11 程序、66 次执行全部匹配 golden |
| Lab6 functional mem2reg | 14 程序、84 次执行全部匹配 golden |
| Lab6 performance all | 3 程序、18 次执行全部匹配 golden；每个子进程 timeout=300 |
| 普通入口附加 oracle | 三个模式各 ABI 3 次、压力 6 次 / 24 子例、双向 C 互调各 5 路、metrics 5 条目标路径 |
| 过滤入口 | baseline-only + stack：10 程序 / 17 输入 / 51 次执行；压力 3 次 / 12 子例，双向 C 互调各 2 路，metrics 2 条目标路径；固定 linear ABI 仍执行 3 次 |
| metrics | legacy + 四 MIR 均 Allocate Size=16、Execute Cost=10，输出 12，与 native LLVM 插桩无关 |
| fail-closed | 10 个实际负例通过：空目录、缺 golden/工具、零/负/实际超时、编译失败、参考错误、目标失败、输出空格错误 |
| 已有中端 | ConstPropagation 14、GVN 18、scaffold 6、LICM 7 组；优化 CLI 7、evaluator 8 项通过 |
| 数据完整性 | Lab6 原始 14 functional + 3 performance、37 个 .cminus/.in/.out 文件逐文件字节与 SHA256 一致 |

Lab6 数据来源 `/Users/qingxuliang/project/compiler/ustc-compiler/lab6/tests/6-register/{functional-cases,performance-cases}`；只引入数据，不引入旧分配器或 eval 脚本。本次全部 17 个程序适用于现有支持子集，无静默跳过、无回退。

同一 isel 与 mem2reg 下的实际静态统计如下。普通 reload/store 仅统计 Allocated 阶段对新增 spill FrameIndex 的访问，排除原 alloca 的程序访存；Final frame 对象中剔除固定 ra/fp 后才计算 callee-saved。ABI save/restore 包含固定 ra/fp 和新增 callee-saved，各函数静态入口/统一出口一次。多函数程序的 frame 列是逐函数 frame size 的合计，不是动态最大栈深度。

| 程序（mem2reg） | isel / allocator | Selected | Final | frame 合计 bytes | spill VRegs | 普通 reload / store | callee-saved | ABI save / restore |
|---|---|---:|---:|---:|---:|---|---:|---|
| recursive | baseline_stack | 23 | 76 | 112 | 17 | 20 / 17 | 0 | 4 / 4 |
| recursive | baseline_linear | 23 | 56 | 48 | 0 | 0 / 0 | 2 | 6 / 6 |
| recursive | greedy_stack | 16 | 56 | 80 | 10 | 13 / 10 | 0 | 4 / 4 |
| recursive | greedy_linear | 16 | 49 | 48 | 0 | 0 / 0 | 2 | 6 / 6 |
| immediates | baseline_stack | 25 | 80 | 96 | 18 | 23 / 18 | 0 | 2 / 2 |
| immediates | baseline_linear | 25 | 49 | 32 | 0 | 0 / 0 | 1 | 3 / 3 |
| immediates | greedy_stack | 21 | 65 | 80 | 14 | 19 / 14 | 0 | 2 / 2 |
| immediates | greedy_linear | 21 | 42 | 32 | 0 | 0 / 0 | 1 | 3 / 3 |

`immediates` 在两种 isel 下 linear 的普通 spill/reload/store 均为 0，且各自比 stack 减少访存和 Final 指令；Selected 在各自 stack/linear 配对中完全相同。`recursive` 的 linear 实际使用并保存 2 个 callee-saved，双向 C 互调与全宽 ABI oracle 验证跨调用行为。压力 oracle 的 linear 寄存器池 0 中四种形状分别 spill 25/26/28/25，池 1 为 15/15/16/15，池 2 均为 9；24 个子例全部实际 spill 并执行已知值断言。

Final 为静态展开计数，`LA_LOCAL` 按 2 条计算。没有收集目标运行耗时或固定加速比；QEMU 执行验证不代表硬件性能。JSON 记录每个程序的四个 MIR 配置、逐函数与合计统计。

现有限制保持：非变参直接调用，整数/指针参数最多 8 个、float 最多 8 个；无栈传参、aggregate、间接调用、动态/非入口 alloca、长分支 relaxation。导出 Cminus 库使用 raw，避免已有全程序 DCE 以 main 为根删除导出函数。已有 LightIR 常量缓存的 Module 生命周期限制未扩大处理范围。linear 是每 VReg 一个保守连续区间、一个位置，不实现区间分裂/空洞/slot 复用。默认 legacy 和 MIR stack 保持。
