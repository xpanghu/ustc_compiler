define i32 @callee(i32 %0) {
    %2 = alloca i32
    store i32 %0, i32* %2
    %3 = load i32, i32* %2
    %4 = mul i32 2, %3
    ret i32 %4
}

define i32 @main() {
    %1 = alloca i32
    store i32 0, i32* %1
    %2 = call i32 @callee(i32 110)
    ret i32 %2
}