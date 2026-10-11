# LoongArch Machine IR 后端

M1、M2 已实现。M1 提供保守指令选择、Machine IR、PHI/ABI lowering、全栈分配、frame lowering 和汇编输出；M2 增加三组局部贪心规则。默认后端仍为 legacy，MIR 后端默认使用 baseline。

## 使用

从仓库根目录运行：

```sh
# M1：保守选择，可配合已有 LightIR 优化
build/cminusfc -S -mem2reg -backend=mir -o example.s example.cminus

# M2：贪心选择，保存三个阶段的快照
build/cminusfc -S -mem2reg -backend=mir -isel=greedy -dump-mir \
  -o example.s example.cminus
```

`-dump-mir` 生成 `example.selected.mir`、`example.prera.mir`、`example.final.mir`；汇编路径还保留现有 `.ll` sidecar。`-backend` 只能用于 `-S`，`-isel`、`-dump-mir`、`-lab4-metrics` 只能用于 MIR 后端。未知选项值、模式冲突及不支持的 ABI 在创建后端输出前报错。

MIR 默认关闭 Lab4 插桩；显式加 `-lab4-metrics` 可生成实际度量调用。此时保留事件顺序、权重和模块 alloca 字节数，禁用比较链及地址链等多节点融合，仍允许单节点立即数选择。

## 阶段与支持范围

```text
LightIR
  → select → Selected verifier / snapshot
  → PHI、ABI、metrics lowering → PreRA verifier / snapshot
  → stack allocation → frame lowering
  → Final verifier / snapshot → assembly printer
```

- 值类型：I1、I32、Ptr64、F32。I1 保持 0/1，用户内存占 1 字节、spill 占 4 字节；I32 运算保持低 32 位的符号扩展表示；浮点常量按位保存。
- 覆盖当前 Cminus 的算术、六种整数及浮点比较、I1→I32 的 zext、I32↔F32 数值转换、入口块中的固定大小 alloca、全局及数组初始化、GEP、load/store、分支、PHI、直接调用及返回。非入口块 alloca 在选择阶段明确拒绝，避免将重复执行的分配错误地复用为同一个栈槽。
- ABI 分别支持最多 8 个整数/指针参数和 8 个 float 参数，支持 void 或单个标量返回。栈参数、可变参数、aggregate、间接调用、动态 alloca 不在本次范围；浮点参数不向整数参数寄存器溢出。
- PHI 在机器层按并行拷贝语义消除；多后继前驱使用对应边块，循环交换和三值环通过显式临时值解决，输入 LightIR 不被改写。
- 全栈分配为实际保留的 VReg 分配 home；普通指令先 reload 所有 use，再执行并 spill def，读写操作数同时建模。保留 `$t0..$t2`、`$ft0..$ft2`；`$t8` 专用于地址和大偏移展开。
- 栈帧按 16 字节对齐，先分配空间再保存 ra/fp；大帧、FrameIndex、常量及 COPY 均在 Final 之前展开。printer 不隐藏 spill 或大偏移处理。`LA_LOCAL` 保留为目标汇编器伪指令。
- verifier 检查操作数、类型、use/def、调用 clobber、CFG、Selected SSA/支配关系、PreRA 可用定义及 Final 残留伪指令。超出分支编码范围明确报错，尚未实现长分支 relaxation。

贪心规则先逆序规划消费者，再按原顺序输出。完整匹配成功后才提交 covered 标记，只吸收同块、单用途的纯节点，遇到 load/store/call 屏障或共享值时回退：

1. I32 add/sub 使用 signed12 `ADDI_W`，减法在 int64 中取负，避免 INT_MIN 溢出。
2. 六种有符号整数比较直接生成条件分支；移除纯 zext/ne0 条件链。对于浮点比较、I1 参数或 I1 load，只消除条件链并保留原生产者。
3. 按类型 stride 计算全常量 GEP 偏移，signed12 范围内折叠进 load/store，否则保留地址计算。

## 构建与测试

```sh
cmake --build build --target cminusfc test_machine_codegen -j6
build/test_machine_codegen all
```

可单独运行 `representation`、`verifier`、`baseline`、`alloca`、`phi`、`calls`、`metrics`、`stack`、`frame`、`printer`、`immediates`、`branches`、`addresses` 分组。`alloca` 构造循环分配并保留前次指针，检查 baseline/greedy 及 metrics 开/关均拒绝该输入且诊断包含函数和块。`emit-interop DIRECTORY` 另行生成供 C 互调测试使用的 LLVM 和两种 MIR 汇编。

本次交叉工具链位于 `jianmu-dev` 容器：仓库挂载到 `/workspace`，`/workspace/build` 是独立构建卷，不能与 macOS 的同名目录二进制混用。

```sh
docker exec jianmu-dev cmake --build /workspace/build \
  --target cminusfc test_machine_codegen -j6
docker exec jianmu-dev /workspace/build/test_machine_codegen all
docker exec jianmu-dev python3 /workspace/tests/backend/test_backend.py \
  --compiler /workspace/build/cminusfc --clang clang --opt opt \
  --cross-cc loongarch64-unknown-linux-gnu-gcc --qemu qemu-loongarch64 \
  --report /tmp/mir-validation.json
```

runner 的 `--mode` 支持 `raw`、`mem2reg`（默认）、`const-prop`、`gvn`、`licm`、`all`。后三个单 pass 模式先运行 mem2reg；`all` 为 mem2reg + const-prop + GVN + LICM。`--baseline-only` 用于 M1 三路验收，`--cli-only` 不需要目标工具链，`--course lab3|lab4` 运行课程材料并检查 `.out` 末行记录的退出状态。每个子进程有超时，工具缺失或执行失败会令测试失败。

LLVM 参考使用 host clang 和源码 `src/io/io.c`；目标路径由交叉 GCC 静态链接同一源码后经 QEMU 执行。测试侧为 `void main` 包装返回 0 的 LLVM 入口。C 调用 MIR 的导出函数测试使用 raw 模式，因为现有全程序 DCE 以 main 为根，会删除无 main 翻译单元中的导出函数。

## 验证记录（2026-10-10）

工具版本：macOS arm64 LLVM 22.1.8（ASAN/UBSAN 构建）；容器 clang/opt 14；LoongArch GCC 12.0.1 20220210；QEMU 6.2.50。M1 验收通过后才实现 M2。

| 验证 | 覆盖与结果 |
|---|---|
| 内部回归 | macOS ASAN/UBSAN 与 Linux Debug 的 `test_machine_codegen all` 全通过 |
| M1 差分 | 9 个程序、14 组输入，LLVM / legacy / baseline 的 stdout 与退出状态一致 |
| M2 差分 | 10 个程序、17 组输入；raw、mem2reg、all 三种前端模式各自四路一致 |
| C 互调 | 双向整数/指针/float 混合调用；49 对浮点比较，含 NaN、±Inf、±0；I1 内存及 ABI 归一化通过 |
| Lab3 | 14 个程序，四路执行均符合课程 stdout/退出状态预期 |
| Lab4 | 11 个程序 × raw/mem2reg/licm = 33 项，四路执行均符合课程预期 |
| 已有专项 | GVN 18 组、ConstPropagation 14 组、scaffold 6 组、LICM 7 组、旧 CLI 7 项、evaluator 8 项全通过；ConstPropagation/GVN native E2E 分别 11/8 个程序通过 |
| 插桩 | legacy/baseline/greedy 均为 Allocate Size=16、Execute Cost=10，程序输出一致 |
| Sanitizer 生成 | 10 个程序 × 2 种选择 × raw/mem2reg/all = 60 次，无后端 sanitizer 报告 |
| 64 位常量 | 11 个 signed12、signed32 及完整 64 位边界/位模式，交叉 C 比较与目标执行通过 |
| CLI | 合法阶段快照、默认行为不变、10 项非法参数/ABI 拒绝、缓冲输出写入错误均通过 |

三组规则的内部矩阵计数如下；额外检查了 6 谓词 × 2 个负常量操作数位置 × 3 个负输入，以及浮点比较/I1 参数/I1 load 三类条件根在 metrics 开/关时的行为。

| 规则 | 用例 | 命中 / 回退 | Selected baseline→greedy | Final baseline→greedy |
|---|---:|---:|---:|---:|
| 立即数 | 20 | 6 / 14 | 80→74 | 372→353 |
| 整数比较分支 | 60 | 24 / 36 | 718→582 | 2028→1644 |
| 常量地址 | 70 | 15 / 55 | 945→825 | 3031→2671 |

默认 mem2reg 的端到端计数：

| 程序 | 输入数 | Selected baseline→greedy | Final baseline→greedy |
|---|---:|---:|---:|
| branch | 2 | 17→12 | 47→33 |
| loop_swap | 3 | 22→16 | 65→48 |
| recursive | 3 | 23→16 | 76→56 |
| mixed_calls | 1 | 27→27 | 92→92 |
| array | 1 | 91→70 | 271→210 |
| float_predicates | 1 | 47→47 | 124→124 |
| float_branch | 3 | 21→17 | 55→44 |
| immediates | 1 | 25→21 | 80→65 |
| large_frame | 1 | 53→28 | 479→251 |
| void_main | 1 | 3→3 | 15→15 |
| 合计 | 17 | 329→257 | 1304→938 |

Selected 计数包含阶段伪指令，Final 为静态展开估计，`LA_LOCAL` 按 2 条计算；不代表最优选择或运行时间收益。`--report` 可以重新保存逐程序 JSON 结果。

实现中捕获并修复的负例包括：`void main` 退出码、I1 字节/参数/返回值归一化、非法转换和 COPY 类型、缺少 false 分支、超过 ABI 参数数、signed12 正负边界、跨屏障融合、共享节点吸收、未使用 VReg 的多余 home，以及 `LU32I_D` 保留低 32 位所需的 read-before-write 描述和 reload/spill。

现有 LightIR 的常量缓存使用 Module 指针作为键，销毁 Module 后重复使用同一地址会碰到过期类型。内部测试保留各 Module 的独立生命周期以避开该已有问题；本次没有修改 LightIR。Lab5 历史 JSON golden 的名称差异不作为本次后端通过证据，已有专项的范围见 [Lab5 README](../5-opt/README.md)。

## Lab6 交接

Machine IR 已暴露带类型的 VReg、use/def/UseDef、显式及隐式固定寄存器、调用 clobber、边块、FrameObject 和 scratch 保留集。下一步可在机器层构建活跃区间并替换全栈分配器为线性扫描；本次实现到 M2，没有增加 Lab6 占位实现。
