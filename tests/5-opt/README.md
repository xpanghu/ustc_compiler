# Lab5：接入状态与使用

本分支已完成当前框架下的保守 **ConstPropagation**；**GVN 算法仍未完成**。
继续使用已完成的 Lab1–4 基础设施；除下述浮点常量缓存和后端比较语义修正外，
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
- `-gvn`、`-dump-json`：开关和 pipeline 位置已接入，但 GVN 明确拒绝执行，
  返回非零退出码与 `Lab5 GVN: algorithm TODOs are not implemented`。
- `-licm`：继续使用已经完成的 Lab4 实现，未改为旧教材 pass。

预期顺序为 Mem2Reg/DCE → ConstPropagation/DCE → GVN/DCE → LICM/DCE，
只运行启用的 pass。GVN 当前在进入 pipeline 前被安全检查拦住。
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
   就替换 PHI 的所有同名 incoming 值。普通 use 仍限定在对应块内。
   替换阶段显式接收 pout，以便独立验证，不开放分析私有状态。
6. 空 partition 不解引用 begin；未完成的 join/intersect 不落到函数末尾；
   run/detectEquivalences 入口有明确的安全占位，不运行不完整迭代。
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
框架适合作为教学起点；这些修正不意味着 leader 支配性、内存副作用建模、
analysis invalidation 或 IR verifier 已经完整。

## 后续练习位置

ConstPropagation 已采用反复扫描实现，不是 SCCP：没有值格、可执行边分析或稀疏工作队列。
保留整数溢出拒绝折叠的安全策略，不实现模 i32 溢出折叠；
内存传播刻意限制在基本块内，不做全局内存数据流或别名分析。
局部变量通常先由 Mem2Reg 转为 SSA；不启用 Mem2Reg 时仍可折叠直接常量表达式。

GVN.hpp / GVN.cpp：表达式类型、join/intersect、数据流初始化与合流、transfer、
valueExpr/valuePhiFunc/getVN、leader 选择和收敛判定。

完成 GVN 后，应移除 `GVN::run` 与 `detectEquivalences` 的入口保护，
以及 main.cpp 的接入期 preflight 检查，只保留 pipeline 中真正的 GVN 执行。
不要只删保护后把现有骨架当成完整算法运行。
GVN 的课程 JSON 名称对应旧前端；先确认块名/值名映射，再解释评分，不能把名称不同视为优化错误。

## 构建与基础设施回归

在项目根目录运行：

```sh
cmake --build build --target cminusfc test_constprop test_lab5_scaffold test_licm -j6
build/test_constprop
build/test_lab5_scaffold
python3 tests/5-opt/test_cli.py build/cminusfc
python3 -m unittest discover -s tests/5-opt -p test_eval.py
build/test_licm

# 11 个程序：raw / optimized 执行、预期输出、IR 结构和 LLVM verifier
python3 tests/5-opt/test_constprop.py \
  --clang /opt/homebrew/opt/llvm/bin/clang --opt /opt/homebrew/opt/llvm/bin/opt
```

当前基础设施测试和默认功能入口只需 Python 标准库。
只有在完成 GVN、读取原 serializer 的 JSON5 输出时才可能需要 json5；
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

# 当前预期失败，待实现 GVN 后才作为算法验收
python3 tests/5-opt/eval_lab5.py --suite gvn-functional --mode optimized \
  --clang /opt/homebrew/opt/llvm/bin/clang
```

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
