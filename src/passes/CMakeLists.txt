add_library(
    passes STATIC
    ConstPropagation.cpp
    DeadCode.cpp
    Dominators.cpp
    FuncInfo.cpp
    GVN.cpp
    LoopDetection.cpp
    LICM.cpp
    Mem2Reg.cpp
    PassManager.cpp)
