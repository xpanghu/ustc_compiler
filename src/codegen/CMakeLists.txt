add_library(
    codegen STATIC
    CodeGen.cpp
    Register.cpp
    MachineIR.cpp
    LoongArchISel.cpp
    MachineLowering.cpp
    StackAllocator.cpp
    LoongArchFrame.cpp
    MachineCodeGen.cpp
)

target_link_libraries(codegen common IR_lib)
