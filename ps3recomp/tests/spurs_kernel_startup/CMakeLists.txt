cmake_minimum_required(VERSION 3.20)
project(spurs_kernel_startup LANGUAGES C)
enable_testing()

# POSIX-only test (pthreads, GCC/Clang sanitizer flags).
if(WIN32)
    message(STATUS "spurs_kernel_startup: POSIX-only test, skipped on Windows")
    return()
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
add_executable(spurs_kernel_startup main.c)
target_include_directories(spurs_kernel_startup PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/spurs"
)
target_link_libraries(spurs_kernel_startup PRIVATE Threads::Threads)
target_compile_options(spurs_kernel_startup PRIVATE
    -Wall -Wextra -Werror -fsanitize=address,undefined
)
target_link_options(spurs_kernel_startup PRIVATE
    -fsanitize=address,undefined
)

add_test(NAME spurs_kernel_startup COMMAND spurs_kernel_startup)
