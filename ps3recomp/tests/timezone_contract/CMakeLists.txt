cmake_minimum_required(VERSION 3.20)
project(timezone_contract LANGUAGES C)

add_executable(timezone_contract main.c)
target_include_directories(timezone_contract PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/syscalls"
)

enable_testing()
add_test(NAME timezone_contract COMMAND timezone_contract)
