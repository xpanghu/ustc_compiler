# Lab5：接入状态与使用

本分支提供当前框架下的保守 **ConstPropagation** 和函数内 **GVN**。
继续使用已完成的 Lab1–4 基础设施；除下述浮点常量缓存、后端比较与 PHI 拷贝修正外，
前端、IR 接口、Mem2Reg、LICM 和相关分析保持不变。

## 来源与版本

源文件来自本地 `/Users/qingxuliang/project/compiler/ustc-compiler/lab5` 的教材配套代码。
课程 GitLab 需要认证，未取得其 Lab5 分支，因此不能称为“已合入 2025 官方 Lab5”。
该来源目录没有 Git 历史，本次在 `work/lab5` 选择性移植，而不是整体覆盖或 Git merge。
源材料中的 baseline 还包含 2020 年路径；它们只说明材料年代，不能用于确认 2025 要求。

移植了 ConstPropagation、GVN 的四个文件，以及 50 个原始测试材料文件，
包括 `.cminus`、`.in`、`.out`、参考 `.ll` 和 GVN 同余类 `.json`。
测试材料保持原始字节不变；没有导入旧 LoopSearch / LoopInvHoist，循环优化使用现有 LICM。
原 eval_lab5 的路径拼接、自动安装、忽略退出码和 Linux 进程计时不适合当前工程，
所以测试入口重新适配，不把它的旧性能评分当成可靠的优化证据。

## 当前能做什么

- `-const-prop`：整数/浮点算术和比较、sitofp/fptosi/zext 常量折叠，
  SSA use 替换、平凡 PHI 简化、常量条件分支和入口不可达块清理。
  上述步骤迭代到不动点，不依赖函数基本块列表的排列顺序。
  同一基本块内支持全局变量常量 store → load 转发；遇到调用、可能别名的写入，
  或对该全局的非常量写入时失效。不会跨基本块传递内存事实或假定可变全局始终等于初始化值。
- ConstFolder：提供整数/浮点算术、比较、数值转换辅助函数；
  已接入 ConstPropagation 的遍历，GVN 也复用这些辅助函数。
- `-gvn`：类型化标量 CSE、PHI 合流与 PHI-of-ops、相同 GEP 地址计算、已证明纯函数调用复用。
  替换检查支配性/指令顺序和 PHI 对应入边；load、alloca、外部或不纯调用保留独立身份。
  不实现通用 load CSE、PRE、浮点重结合或跨函数优化。
- `-dump-json`：向当前工作目录的 `gvn.json` 写入收敛后、替换前的严格 JSON 快照。
  i1/i32 分别输出布尔/整数；其他常量输出带类型的字符串。写入失败返回非零退出码。
- `-licm`：继续使用已经完成的 Lab4 实现，未改为旧教材 pass。

预期顺序为 Mem2Reg/DCE → ConstPropagation/DCE → GVN/DCE → LICM/DCE，
只运行启用的 pass。GVN 只在正式 pipeline 中运行一次；后置 DCE 清理冗余指令。
`-gvn` 必须配合 `-mem2reg`，`-dump-json` 必须配合 `-gvn`。

## 对基础代码做了哪些必要修正

1. 旧 `Pass` 改为当前 `TransformPass`；intrusive list 的对象遍历改为指针容器遍历。
   使用当前 DeadCode 构造接口，不重新引入 LLVM 链接依赖。
2. 浮点比较返回 `i1` 常量。按照当前 LightIR 输出的 unordered predicates，
   NaN 参与比较时结果为 true，不照搬另一版本的 ordered 语义。
3. 整数折叠先用 i64 计算，i32 溢出时拒绝折叠并保留原指令；
   除零、INT_MIN / -1、NaN/无穷/越界浮点转整数也拒绝折叠。
   这不是完整的任意位宽或模整数折叠实现。
4. GVN 的 folder 复用修正后的 ConstFolder，避免两个 pass 的边界行为不同。
5. PHI 替换只作用于 value operand 配对的 incoming block，不能因为目标块是某个前驱，
   就替换 PHI 的所有同名 incoming 值。代表还须在对应使用点可用。
   替换阶段显式接收 pout，以便独立验证，不开放分析私有状态。
6. TOP 与空事实区分，partition 收敛判断不依赖展示编号；循环 PHI 按入边并行投影。
7. ConstPropagation 的 folder 为值成员；丢弃的指令使用 erase_instr 删除，
   而非仅 detach 后留下悬空 use 或泄漏。
8. 旧递归 CFG 清理及入口判断占位已由入口可达性搜索替代，能删除不可达环。
   分支改写先清理被丢弃边对应的 PHI 输入，先析构旧分支再创建新分支。
   批量删块先删除所有死块的分支，再 detach/delete 块，保证 CFG 析构时目标仍存活。
9. 浮点常量按位模式缓存，修正数值 float 键合并 +0/-0、无法识别 NaN 键的问题。
   PHI 常量相等判断也保留浮点位模式和类型差异。
10. NaN 端到端回归发现原 LoongArch 后端使用 ordered 比较，与 LightIR 的 unordered
    比较不一致。只修正 gen_fcmp 的六个谓词映射为 SULE/SULT/SUEQ/SUNE，
    保留原操作数顺序与 signalling 形式，不改变 IR 或语言的比较语义。
    对照依据为[龙芯官方手册比较条件表](https://loongson.github.io/LoongArch-Documentation/LoongArch-Vol1-EN.html#_fcmp_cond_s_d)。

表达式类、同余类和数据流迭代的课程组织保留。
内存分析仍不等价于完整别名分析，各 pass 自行建立所需分析。

嵌套循环的 PHI 交换回归发现原 LoongArch `copy_stmt` 按顺序写目标栈槽，
`a ← b; b ← a` 会让第二次读取已覆盖的 a；问题在 mem2reg 模式已出现，GVN 前后 IR 相同。
后端现为每个 PHI 分配临时栈槽，在写任何目标之前暂存全部源，再统一复制到目标。
另一个直接 SSA 条件自回边回归发现未选择的回边复制也会覆盖出口仍需使用的 PHI 值：
循环退出应返回 3，原后端返回 4。现在先判断分支条件，使用局部汇编标签进入所选边，
仅复制该边的 PHI 输入；原 IR CFG 不变。
修改只涉及 PHI 复制和所需栈槽/偏移访问；寄存器分配与控制流保持原状。
此修正增加 PHI 边的临时内存复制，不宣称改善后端性能。

## 后续练习位置

ConstPropagation 已采用反复扫描实现，不是 SCCP：没有值格、可执行边分析或稀疏工作队列。
保留整数溢出拒绝折叠的安全策略，不实现模 i32 溢出折叠；
内存传播刻意限制在基本块内，不做全局内存数据流或别名分析。
局部变量通常先由 Mem2Reg 转为 SSA；不启用 Mem2Reg 时仍可折叠直接常量表达式。

GVN 保持保守内存边界：相同地址可以复用，store 前后的 load 不合并。
循环状态只保存有限的成员关系和表达式键；旧操作数没有存活名称时丢弃该表达式事实，
不递归展开 PHI，不证明一般归纳等式。无法追踪指针来源的函数调用也保守拒绝复用。
GVN 内部 18 组回归覆盖类型/位模式、状态契约、可用性、三前驱、多回边、并行 PHI、
合流块区别、PHI-of-ops、常量折叠后事实保留、副作用、不可达入边和 JSON 转义。
PassManager 使用独占所有权，即使 pass 抛出异常也释放所有 pass；main 报告运行错误并返回非零。

GVN 的课程 JSON 名称对应旧前端；先确认块名/值名映射，再解释评分，不能把名称不同视为优化错误。

## 构建与基础设施回归

在项目根目录运行：

```sh
cmake --build build --target cminusfc test_gvn test_constprop test_lab5_scaffold test_licm -j6
build/test_gvn all
build/test_constprop
build/test_lab5_scaffold
python3 tests/5-opt/test_cli.py build/cminusfc
python3 -m unittest discover -s tests/5-opt -p test_eval.py
build/test_licm

# 11 个程序：raw / optimized 执行、预期输出、IR 结构和 LLVM verifier
python3 tests/5-opt/test_constprop.py \
  --clang /opt/homebrew/opt/llvm/bin/clang --opt /opt/homebrew/opt/llvm/bin/opt
```

GVN 端到端专项覆盖 8 个回归：7 个 Cminus 程序比较 raw、mem2reg、mem2reg+gvn，
并运行 const-prop+gvn+licm 组合；另一个直接 SSA 构造用例比较 raw/GVN，
覆盖条件自回边和整数/浮点 PHI 交换。所有 IR 经 verifier，分支运行两侧输入，循环运行 0/1/5 次。
脚本打印工具版本和 mem2reg/GVN 的指令计数，同时检查标量链、PHI-of-ops、
纯调用、地址计算确实消除冗余，以及副作用和独立 load 保留。

```sh
python3 tests/5-opt/test_gvn.py \
  --clang /opt/homebrew/opt/llvm/bin/clang --opt /opt/homebrew/opt/llvm/bin/opt
docker exec jianmu-dev python3 /workspace/tests/5-opt/test_gvn.py \
  --compiler /workspace/build/cminusfc --target loongarch
```

native（LLVM 22.1.8）与 LoongArch/QEMU（LLVM 14、GCC 12.0.1、QEMU 6.2.50）
均通过 8 个回归的输出和 verifier 检查。两条路径的 mem2reg → GVN 计数一致：

| 程序 | 优化证据 | 保留行为 |
| --- | --- | --- |
| 标量链 | add 2 → 1，mul 2 → 1 | input 2 次 |
| 菱形 PHI | add 3 → 2，PHI 3 → 1 | 两侧分支结果一致 |
| 循环 | add 5 → 4 | PHI 2 个，0/1/5 次输出正确 |
| 嵌套循环交换 | PHI 6 个保留 | 0/1/5 次输出正确 |
| 直接 SSA PHI | 条件自回边，整数/浮点交换 | raw/GVN 输出 3、2、1、2.5、1.5 |
| 纯/不纯调用 | pure 2 → 1 | input/read 各 2 次 |
| 数组地址 | GEP 8 → 3 | load 4 次，alias store 后值正确 |
| 浮点边界 | fadd 2 → 1 | 负零、NaN 比较结果正确 |

未测量生成程序运行速度，不据此宣称加速。

脚本启动时检查 LLVM opt、本机 clang 或交叉 GCC/QEMU 是否存在，缺少工具时明确失败。
当前 GVN dump 和测试只需 Python 标准库。
读取历史 JSON5 文件时才可能需要 json5；
请在自己的虚拟环境显式安装，例如 `uv pip install json5`，脚本不会自动安装。

## 课程用例检查

```sh
# 18 个用例：10 个检查执行输出与 baseline，8 个 GVN 功能用例只检查 IR 可编译
python3 tests/5-opt/eval_lab5.py --suite all --mode mem2reg \
  --clang /opt/homebrew/opt/llvm/bin/clang

# 4 个原始常量传播用例：优化后执行输出与课程 baseline 一致
python3 tests/5-opt/eval_lab5.py --suite const-prop --mode optimized \
  --clang /opt/homebrew/opt/llvm/bin/clang

# 4 个旧教材循环优化用例：使用当前 Lab4 LICM
python3 tests/5-opt/eval_lab5.py --suite licm --mode optimized \
  --clang /opt/homebrew/opt/llvm/bin/clang

# 旧参考的块名/值名差异必须单独解释，不能覆盖 golden
python3 tests/5-opt/eval_lab5.py --suite gvn-functional --mode optimized \
  --clang /opt/homebrew/opt/llvm/bin/clang
```

本次旧课程 functional 的 JSON 比较为 **0/8**，但 8 份生成 IR 均通过 LLVM verifier。
参考使用 `label_entry/labelN`，当前使用 `main_entry/main_N`；用例 6、8 的 main 块数还分别
从参考的 15、33 变为当前的 7、13，不能建立简单一一名称映射。
当前 dump 也包含类型化常量、参数/全局边界事实及替换前指令，因此不能把旧 partition 完全相同
作为当前算法正确性的唯一判据。golden 保持原始字节，未用新输出覆盖。
下面是同一当前前端下 mem2reg → GVN 的结构证据，详细计数保存于
`output/gvn-plan-validation/summary.json`：

| 旧 functional 用例 | mem2reg → GVN |
| --- | --- |
| 1 | add 2 → 1 |
| 2 | add 7 → 0，PHI 3 → 1；两次 output 使用同一 PHI |
| 3 | add 4 → 3，PHI 3 → 1；两次 output 使用同一 PHI |
| 4 | add 7 → 4 |
| 5 | add 8 → 6，PHI 7 → 5 |
| 6 | GEP 7 → 6，call 7 → 6 |
| 7 | sdiv 5 → 4，call 14 → 11，PHI 10 → 4；结果复用同一 PHI |
| 8 | add 40 → 37，mul 14 → 11，GEP 34 → 33，call 49 → 48 |

旧 performance 两例执行输出 **2/2** 正确；没有测量时间，不提供加速结论。

也可以 `sh tests/5-opt/eval_lab5.sh ...`。默认用本机构建的编译器和运行库，
默认输出放在忽略的 `tests/5-opt/output/`，脚本可从任何工作目录调用。
每个用例每次运行使用新的独立子目录，保留结果便于检查，不能复用上次的 IR 或 JSON。
可显式指定 `--compiler`、`--runtime`、`--clang`、`--output-dir`。
不要将本机 arm64 编译器/运行库与容器 x86_64 的构建混用。
这部分验证 IR 优化，不必经过 LoongArch QEMU。

测试侧会给 void main 增加 i32 main 包装，只修改生成物，以定义本机退出状态；
baseline_compat.c 仅为旧 baseline 提供缺失的负数组下标异常符号，触发异常即失败。
当前前端、运行库和原始测试材料不会因此修改。
功能入口检查每个子进程的退出码、超时和输出；不测进程启动时间，不输出性能评分。

LoongArch 后端回归仍使用已安装的容器（本次名称为 `jianmu-dev`）：

```sh
docker exec jianmu-dev sh -lc 'cmake --build /workspace/build -j6'
docker exec jianmu-dev /workspace/build/test_constprop
docker exec jianmu-dev python3 /workspace/tests/5-opt/test_constprop.py \
  --compiler /workspace/build/cminusfc --target loongarch
docker exec -w /workspace/tests/4-opt jianmu-dev /workspace/build/eval_lab4 all testcases debug
```

旧 eval_lab4 会在失败后仍返回零，所以还必须确认 all_log.txt 的 33 项结果全部为 OK。

ConstPropagation 专项内部回归为 14 组，检查所有整数运算/比较、浮点运算/比较、
NaN/有符号零、非法折叠保留、PHI、逆序基本块、不动点、被删除的可达边、同目标分支、
不可达循环、循环变化值、内存屏障，并检查 CFG/支配性/双向 use-list 和重复运行不变。
端到端 11 个程序在 native LLVM 与 LoongArch/QEMU 两条路径均通过 raw/optimized 对照，
每份生成 IR 均经过 LLVM verifier。此处只验证正确性和优化确实发生，不提供性能结论。
NaN 回归覆盖左右两侧 NaN 和全部六种比较；有限值回归覆盖小于、等于、大于，
避免仅用 NaN 全真结果掩盖后端操作数顺序错误。
