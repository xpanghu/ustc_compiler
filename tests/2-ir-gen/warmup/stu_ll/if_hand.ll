define i32 @main() {
    %1 = alloca i32
    %2 = alloca float
    store i32 0, i32* %1
    store float 0x40163851E0000000, i32* %2
    %3 = load float, float* %2
    %4 = fcmp ogt float %3, 1.000000e+00
    br i1 %4, label %5, label %6

5:
    store i32 233, i32* %1
    br label %7

6:
    store i32 0, i32* %1
    br label %7

7:
    %8 = load i32, i32* %1
    ret i32 0
}