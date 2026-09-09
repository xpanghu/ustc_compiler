add_executable(
    cminusfc
    main.cpp
)

target_link_libraries(
    cminusfc
    common
    syntax
)

# GCC 8 及更早版本需要显式链接 stdc++fs 才能使用 <filesystem>；
# 其它工具链（如 macOS 上的 clang/libc++) 没有这个库，链接会失败。
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_libraries(cminusfc stdc++fs)
endif()

install(
    TARGETS cminusfc
    RUNTIME DESTINATION bin
)
