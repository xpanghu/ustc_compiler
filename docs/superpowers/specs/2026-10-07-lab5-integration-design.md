# Lab5 基础代码接入方案

## 目标与边界

在现有 Lab1–4 的实现上创建 `work/lab5`，接入 Lab5 的课程基础代码、
构建入口和测试材料，并修正接入必需的兼容性与安全问题。
本次不完成 Lab5 的常量传播与 GVN 算法，不引入 Machine IR、指令选择或 Lab6。

Lab4 的五项未提交改动先保存到 `work/lab4`，新分支从该提交开始。
不推送远程，不修改 `main`，不处理既有分支与远程之间的分歧。

## 来源

使用本地 `/Users/qingxuliang/project/compiler/ustc-compiler/lab5` 的教材配套基础代码。
该目录没有 Git 历史。当前课程 GitLab 需要认证，无法获取其 Lab5 分支，
因此本次是选择性移植，不是对已验证的 2025 课程 Lab5 分支做 Git merge。
接入记录必须明确这一来源和版本限制。

## 方案选择

1. 选择性移植新增 pass 和测试：保留现有实现，修改范围可控，采用此方案。
2. 整体覆盖为旧教材 Lab5：会覆盖已完成的前端、后端与 LICM，不采用。
3. 等待课程 GitLab 的访问权限：来源最准确，但当前不能获取，不作为本次前提。

## 文件与接口

新增课程文件：

- `include/passes/ConstPropagation.hpp`、`src/passes/ConstPropagation.cpp`。
- `include/passes/GVN.hpp`、`src/passes/GVN.cpp`。
- `tests/5-opt` 的课程用例及必要测试入口。

修改现有 `src/passes/CMakeLists.txt`、`src/cminusfc/main.cpp` 和
`tests/CMakeLists.txt`，接入构建及优化开关。
保留现有 LightIR、Mem2Reg、Dominators、FuncInfo、LoopDetection、LICM 和 CodeGen。
不移植旧 `LoopInvHoist`、`LoopSearch` 或旧项目根 CMake 配置。

课程 pass 从旧 `Pass` 适配到当前 `TransformPass`；
遍历从 LLVM intrusive list 适配到当前存放对象指针的标准容器。
DeadCode 使用当前需要显式指定是否删除不可达块的构造接口。
不为旧容器重新引入 LLVM 链接依赖。

## 必须修正的基础代码问题

- 常量传播的浮点比较必须产生 `i1`，不能返回浮点常量。
- 常量折叠避免宿主 C++ 的有符号整数溢出、除零、最小整数除以负一、
  非法浮点转整数等未定义行为；不能安全折叠的表达式保留原状。
- GVN 的 PHI 替换按当前 value operand 对应的 incoming block 判断，
  不能只判断某块是否出现在 PHI 所在块的前驱列表中。
- `isTop` 对空 partition 安全返回 false。
- 有返回值但尚未实现的函数不能落到函数末尾；
  未完成的 GVN 必须明确报错，不执行可能不收敛的半成品数据流迭代。
- ConstPropagation 的辅助对象使用明确的资源生命周期。
- 适配当前指令删除、操作数 use-list 和 CFG 接口；
  不照搬旧容器的删除方式，不提前实现尚未要求的分支简化算法。

教学中的表达式类、同余类、数据流迭代结构及算法练习位置保留。
基础代码适合作为教学起点，不视为完整、可直接信赖的优化器。
修正上述问题不代表 GVN 的 leader 支配性、内存副作用或分析失效机制已完整实现。

## 测试与工具

新增测试先验证接口未接入时失败，再验证适配后的构建、常量折叠边界、
PHI incoming edge 替换和未完成 GVN 的明确失败行为。
现有优化开关和默认编译路径保持不变。

课程测试入口显式指定编译器和输出目录，检查进程返回值与输出；
不自动安装 Python 包，不向课程用例目录写入生成物，不依赖 Linux `taskset`。
IR 优化功能测试可以使用本机 LLVM 工具，不强制通过 LoongArch QEMU。
课程评分文件的值名称与当前前端可能不同，不能把名称不匹配直接等同于优化错误。
原有性能脚本的进程计时也不作为可靠的后端性能结论。

## 验收标准

1. Lab4 的五项修改存在于 `work/lab4` 的独立提交；`work/lab5` 从它创建。
2. 新增 pass 可以在当前工程编译，编译器入口可识别相应开关。
3. 新增基础设施回归测试通过，未完成算法明确标识，GVN 不崩溃或死循环。
4. 本机构建、现有 LICM 七组回归测试，以及 Lab4 的 33 次课程测试通过。
5. 报告区分“基础代码接入完成”和“Lab5 算法完成”；不宣称后者。
6. 交付来源说明、必要修改说明和可复现的测试方法；不推送远程。
