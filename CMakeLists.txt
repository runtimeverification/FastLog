cmake_minimum_required(VERSION 3.10)
project(FastLog)

# TODO: it appears that using C++17 would generate less efficient code...
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-stack-protector -mprfchw")

add_executable(FastLog Main.cc BufferManager.cc Context.cc)
target_link_libraries(FastLog pthread)