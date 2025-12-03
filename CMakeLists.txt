cmake_minimum_required( VERSION 3.8 )
include(CheckCXXCompilerFlag)

project(CMINUSF)
add_compile_definitions(_POSIX_C_SOURCE=200112L)

set(CMAKE_C_FLAGS "${CMAKE_CXX_FLAGS} -std=c99")

SET(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -Wall -g2 -ggdb")
SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall")
SET(CMAKE_CXX_FLAGS_ASAN "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=undefined -fsanitize=address")

set(default_build_type "Debug")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        check_cxx_compiler_flag("-fstandalone-debug" CXX_SUPPORTS_STANDALONE_DEBUG)
        if(CXX_SUPPORTS_STANDALONE_DEBUG)
            message(STATUS "Adding -fstandalone-debug for Clang")
            set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fstandalone-debug")
        else()
            message(STATUS "Clang does not support -fstandalone-debug, skipping")
        endif()
    else()
        message(STATUS "Use Gcc")
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions(-DDEBUG_BUILD)
endif()

if(NOT(CMAKE_BUILD_TYPE_SHADOW STREQUAL CMAKE_BUILD_TYPE))
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        message(STATUS "Setting build type to '${default_build_type}'")
        set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE STRING "Choose the type of build." FORCE)
    else()
        message(STATUS "Building in ${CMAKE_BUILD_TYPE} mode")
    endif()

    set(CMAKE_BUILD_TYPE_SHADOW ${CMAKE_BUILD_TYPE} CACHE STRING "used to detect changes in build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)

INCLUDE_DIRECTORIES(
    include
    include/cminusfc
    include/common
    include/lightir
    include/codegen
    include/passes
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_BINARY_DIR})
add_subdirectory(src)
add_subdirectory(tests)
