add_library(common STATIC
    syntax_tree.c
    ast.cpp
    logging.cpp
    util.cpp)

target_link_libraries(common)

