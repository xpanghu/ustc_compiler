add_executable(
    cminusfc
    main.cpp
    cminusf_builder.cpp
)

target_link_libraries(
    cminusfc
    IR_lib
    common
    codegen
    syntax
    passes
)

# std::filesystem 在 GCC 9 / Clang 9 之前位于独立的 stdc++fs (libstdc++) 或 c++fs (libc++) 库里,
# 之后已并入标准库。新编译器上强行链接会直接报 "library 'stdc++fs' not found"。
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
    target_link_libraries(cminusfc PRIVATE stdc++fs)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
    target_link_libraries(cminusfc PRIVATE c++fs)
endif()

install(
    TARGETS cminusfc
    RUNTIME DESTINATION bin
)
