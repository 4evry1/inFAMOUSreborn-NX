cmake_minimum_required(VERSION 3.20)
project(host_file_contract LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(host_file_contract
    main.cpp
    ../../runtime/platform/host_probe.c
)
target_include_directories(host_file_contract PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/platform"
)

enable_testing()
add_test(NAME host_file_contract COMMAND host_file_contract)
