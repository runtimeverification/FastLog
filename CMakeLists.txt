cmake_minimum_required(VERSION 3.10)
project(FastLog)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-stack-protector -mprfchw")

add_executable(FastLog Main.cc BufferManager.cc Context.cc)
target_link_libraries(FastLog pthread)