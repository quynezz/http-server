cmake_minimum_required(VERSION 3.10)

# Project name and version
project(HttpServer VERSION 1.0 LANGUAGES C)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Add executable
add_executable(http_server http_server.c)

# Find and link pthreads
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(http_server PRIVATE Threads::Threads)

# Include directories
target_include_directories(http_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Compiler flags
if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(http_server PRIVATE -Wall -Wextra -pthread)
endif()
