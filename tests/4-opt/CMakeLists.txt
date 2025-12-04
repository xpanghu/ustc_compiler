add_executable(
    eval_lab4
    eval_lab4.cpp
)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
    target_link_libraries(eval_lab4 PRIVATE stdc++fs)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
    target_link_libraries(eval_lab4 PRIVATE c++fs)
endif()

install(
    TARGETS eval_lab4
    RUNTIME DESTINATION bin
)