cmake_minimum_required(VERSION 3.20)
project(filesystem LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_path(LIBURING_INCLUDE_DIR NAMES liburing.h)
find_library(LIBURING_LIBRARY NAMES liburing uring)

if (NOT LIBURING_INCLUDE_DIR OR NOT LIBURING_LIBRARY)
    message(FATAL_ERROR "liburing is required for the async I/O layer")
endif()

add_library(filesystem_core
    src/async_io.cpp
    src/block_device.cpp
    src/disk_format.cpp
    src/bitmap.cpp
    src/buffer_pool.cpp
    src/extent.cpp
)

target_include_directories(filesystem_core PUBLIC include ${LIBURING_INCLUDE_DIR})
target_link_libraries(filesystem_core PUBLIC ${LIBURING_LIBRARY})
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

add_executable(filesystem-async-io-demo demo/async_io_demo.cpp
)
target_link_libraries(filesystem-async-io-demo PRIVATE filesystem_core)
target_compile_options(filesystem-async-io-demo PRIVATE -Wall -Wextra -Wpedantic)

add_executable(filesystem-extent-demo demo/extent_demo.cpp)
target_link_libraries(filesystem-extent-demo PRIVATE filesystem_core)
target_compile_options(filesystem-extent-demo PRIVATE -Wall -Wextra -Wpedantic)

add_executable(filesystem-buffer-pool-demo demo/buffer_pool_demo.cpp)
target_link_libraries(filesystem-buffer-pool-demo PRIVATE filesystem_core)
target_compile_options(filesystem-buffer-pool-demo PRIVATE -Wall -Wextra -Wpedantic)

add_executable(filesystem-metadata-demo demo/metadata_demo.cpp)
target_link_libraries(filesystem-metadata-demo PRIVATE filesystem_core)
target_compile_options(filesystem-metadata-demo PRIVATE -Wall -Wextra -Wpedantic)
