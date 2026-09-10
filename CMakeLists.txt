cmake_minimum_required(VERSION 3.20)
project(filesystem LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(filesystem_core
    src/block_device.cpp
    src/disk_format.cpp
    src/bitmap.cpp
)

target_include_directories(filesystem_core PUBLIC include)
target_compile_options(filesystem_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(filesystem main.cpp)
target_link_libraries(filesystem PRIVATE filesystem_core)
target_compile_options(filesystem PRIVATE -Wall -Wextra -Wpedantic)

add_executable(filesystem-mkfs tools/mkfs.cpp)
target_link_libraries(filesystem-mkfs PRIVATE filesystem_core)
target_compile_options(filesystem-mkfs PRIVATE -Wall -Wextra -Wpedantic)

add_executable(filesystem-dumpfs tools/dumpfs.cpp)
target_link_libraries(filesystem-dumpfs PRIVATE filesystem_core)
target_compile_options(filesystem-dumpfs PRIVATE -Wall -Wextra -Wpedantic)
