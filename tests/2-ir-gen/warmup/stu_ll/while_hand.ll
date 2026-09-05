define i32 @main() {
    %1 = alloca i32
    %2 = alloca i32
    %3 = alloca i32
    store i32 0, i32* %1
    store i32 10, i32* %2
    store i32 0, i32* %3
    br label %4

4:
    %5 = load i32, i32* %3
    %6 = icmp slt i32 %5, 10
    br i1 %6, label %7, label %13

7:
    %8 = load i32, ptr %3
    %9 = add i32 %8, 1
    store i32 %9, i32* %3
    %10 = load i32, ptr %2
    %11 = load i32, ptr %3
    %12 = add i32 %10, %11
    store i32 %12, ptr %2
    br label %4

13:
    %14 = load i32, i32* %2
    ret i32 %14
}