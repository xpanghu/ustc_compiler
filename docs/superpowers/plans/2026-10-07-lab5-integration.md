# Lab5 Scaffold Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** 接入可编译、可测试的 Lab5 基础代码，不完成优化算法。

**Architecture:** 从本地教材 Lab5 选择性移植四个 pass 文件和测试材料。
保持当前指针容器及 TransformPass 接口，提供独立的基础设施回归与功能测试入口。
ConstPropagation 仅保留原有整数算术折叠基线，GVN 在未完成分析入口明确抛出异常。

**Tech Stack:** C++17、CMake/Ninja、Python 3 标准库、本机 LLVM、LoongArch QEMU。

## Global Constraints

- Spec: `docs/superpowers/specs/2026-10-07-lab5-integration-design.md`，已由用户确认。
- 当前分支为 `work/lab5`，源于 Lab4 提交 `f495aa1`；不推送远程。
- 不覆盖 LightIR、LICM、LoopDetection、Mem2Reg 或 CodeGen。
- 不引入 LLVM 链接依赖、Machine IR、指令选择或 Lab6。
- 不自动安装 Python 包，不在测试材料目录生成输出。
- 对算法未完成与功能回归通过分别报告。

## Task 1: 接入 pass 与编译器开关

**Files:**
- Create: `include/passes/ConstPropagation.hpp`, `src/passes/ConstPropagation.cpp`
- Create: `include/passes/GVN.hpp`, `src/passes/GVN.cpp`
- Create: `tests/5-opt/test_cli.py`, `tests/5-opt/scaffold_regression.cpp`, `tests/5-opt/CMakeLists.txt`
- Modify: `src/passes/CMakeLists.txt`, `src/cminusfc/main.cpp`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `TransformPass(Module*)`, pointer-list LightIR containers, `DeadCode(Module*, bool)`。
- Produces: `ConstPropagation(Module*)`, `GVN(Module*, bool)`、`-const-prop`、`-gvn`、`-dump-json`。
- Replacement stage: `void GVN::replace_cc_members(const std::map<BasicBlock*, partitions>&)`。
  原无参方法改为显式接收分析结果，不暴露私有状态即可独立验证 PHI 替换。

- [x] 写 CLI 回归：帮助输出识别新开关；`(2+3)*4` 经整数折叠返回 20；
  未完成 GVN 非零退出、输出明确诊断且不创建结果文件；`-dump-json` 依赖 `-gvn`。
- [x] Run: `python3 tests/5-opt/test_cli.py build/cminusfc`
  Expected: 新开关未接入，回归失败。
- [x] 原样导入四个文件和课程用例，先进行容器、基类、构造参数适配。
  核心构建增量：
  ```cmake
  # src/passes/CMakeLists.txt 中的 passes 源文件列表
  ConstPropagation.cpp
  GVN.cpp
  ```
- [x] 写 C++ 回归覆盖比较结果类型、NaN 比较、整数溢出/非法除法、非法浮点转换、
  GVN 空 partition、PHI 两条边含相同值时的精确替换、未完成算法抛错。
  检查常量折叠拒绝非法表达式，例如：
  ```cpp
  check(folder.compute(Instruction::sdiv, one, zero) == nullptr, "division by zero");
  check(folder.compute(Instruction::fne, nan, fp_one)->get_type() == m.get_int1_type(), "fcmp result type");
  ```
- [x] Run: `cmake --build build --target test_lab5_scaffold -j6 && build/test_lab5_scaffold`
  Expected: 原始浮点比较结果类型或 PHI 替换断言失败；观察失败后修正提供代码。
- [x] 修正折叠、PHI incoming edge、资源生命周期和未实现返回值。
  `join` / `intersect` 的安全占位形式：
  ```cpp
  throw std::runtime_error("Lab5 GVN: join TODO is not implemented");
  ```
  不完整的 `detectEquivalences` 在修改数据流状态前抛错；保留其课程迭代结构。
  旧 CFG 清理辅助函数保留练习接口但显式抛错，避免未来填分支 TODO 后误用旧删除逻辑。
- [x] CLI 中 GVN 提前检查失败并转为 stderr 诊断，避免 PassManager 原始指针异常路径泄漏；
  完整算法完成后应删除该接入期检查，并改为正常加入 pass pipeline。
- [x] Run: `cmake --build build --target cminusfc test_lab5_scaffold test_licm -j6 && build/test_lab5_scaffold && python3 tests/5-opt/test_cli.py build/cminusfc && build/test_licm`
  Expected: 构建、基础设施和 CLI 回归以及七组 LICM 回归通过。

## Task 2: 可复现的课程功能测试入口与交付说明

**Files:**
- Create: `tests/5-opt/eval_lab5.py`, `tests/5-opt/eval_lab5.sh`, `tests/5-opt/test_eval.py`
- Create: `tests/5-opt/.gitignore`, `tests/5-opt/README.md`
- Create: `tests/5-opt/testcases/**`，从原目录逐文件原样导入。

**Interfaces:**
- Consumes: Task 1 的编译器开关和当前 `build/libcminus_io.a`。
- Produces: evaluator 的 `--compiler`, `--clang`, `--runtime`, `--output-dir`, `--suite`, `--mode` 参数。
- Suites: `const-prop`, `licm`, `gvn-functional`, `gvn-performance`, `all`。
- Modes: `raw`, `mem2reg`, `optimized`。GVN functional 的非 optimized 模式只验证生成 IR，
  optimized 模式保留同余类检查入口；明确名称依赖，不提供不可靠的自动重命名。

- [x] 写 evaluator 回归：非零进程状态不能通过；路径含空格可用；没有用例必须失败；
  生成物限制在输出目录中；GVN 未完成返回失败而不是空的成功评分。
- [x] Run: `python3 -m unittest discover -s tests/5-opt -p test_eval.py`
  Expected: evaluator 尚未提供，回归失败。
- [x] 用标准库实现参数与安全子进程入口：
  ```python
  subprocess.run(command, cwd=artifact_dir, input=input_bytes,
                 capture_output=True, timeout=60, check=True)
  ```
  command 为参数列表；显式 `-o artifact.ll`，输入/答案由用例位置读取。
  baseline IR 在同一输出区域编译并运行，不计进程启动时间，不自动安装第三方包。
- [x] Run: `python3 -m unittest discover -s tests/5-opt -p test_eval.py`
  Expected: evaluator 回归通过。
- [x] Run: `python3 tests/5-opt/eval_lab5.py --suite all --mode mem2reg --clang /opt/homebrew/opt/llvm/bin/clang`
  Expected: 18 个课程用例完成基线/输入设施验证；GVN functional 只验证 IR，不称为 GVN 算法测试。
- [x] Run: `python3 tests/5-opt/eval_lab5.py --suite const-prop --mode optimized --clang /opt/homebrew/opt/llvm/bin/clang`
  Expected: 四个用例输出正确，不等于完整常量传播已经实现。
- [x] Run: `docker exec -w /workspace/tests/4-opt jianmu-lab3 /workspace/build/eval_lab4 all testcases debug`
  Expected: 容器重新构建后 33 次课程回归通过；检查日志而非仅依赖退出码。
- [x] README 写明来源、基线能力、保留练习位置、基础代码修正、功能与评分/性能的区别。
- [x] 对照 spec 检查差异与测试记录，显式暂存本次文件并提交；不推送。

## Progress

- Task 1: complete
- Task 2: complete
- 已决定按用户要求在当前 `work/lab5` 继续，不创建另一工作树。
- Ruling: 浮点比较遵循当前 LightIR 的 unordered predicates；NaN 时六种比较均为 true，
  不能照搬另一 IR 版本的 ordered 语义。由 `src/lightir/IRprinter.cpp` 确认。
- Ruling: 教材允许 void main；测试生成物增加 i32 main 包装，定义本机退出状态，
  不忽略非零退出，不修改原始用例或基线。
- Ruling: 旧 baseline 调用 neg_idx_except，当前运行库没有此符号；
  测试侧新增 baseline_compat.c，仅链接到旧 baseline，越界视为失败。
- Ruling: GVN 原输出为 JSON5；只有完成 GVN 后的评分入口才按需读取 json5，
  不自动安装，默认基础设施测试无需第三方 Python 包。
- Ruling: 现有实验容器已名为 jianmu-dev；验证挂载与工具后复用，不重建或改名。

## Final verification

- Native ASAN+UBSAN build: success; scaffold 6 groups, CLI 6 tests, evaluator 7 tests, LICM 7 groups passed.
- Course checks: 18/18 in mem2reg mode (10 runtime+baseline checks, 8 GVN IR-only checks).
- ConstPropagation integer-folding baseline: 4/4; existing LICM: 4/4 old textbook cases.
- Current LoongArch Lab4: 33/33 OK in jianmu-dev.
- GVN optimized: intentionally rejected 8/8 cases with explicit diagnostics; this is NOT an implemented pass.
- All 50 fixture files byte-match their local source.
- Independent read-only review: approved after stale-artifact regression fix; no unresolved Critical/Important issues.
- Ruling: fresh per-case output directories prevent stale IR/JSON from producing false success; duplicate congruence-class multiplicity is preserved by Counter.
- Keep work/lab5 locally; no merge to main, no remote push.
- Fixture whitespace exception: the imported sources contain four pre-existing trailing-whitespace lines.
  Preserve their original bytes; whitespace checks for implementation/docs exclude tests/5-opt/testcases.
