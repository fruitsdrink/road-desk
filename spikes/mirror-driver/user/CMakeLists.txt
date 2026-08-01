cmake_minimum_required(VERSION 3.20)
project(road-desk-mirror-probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W3 /utf-8 /wd4996)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(mirror_probe mirror_probe.cpp)
target_include_directories(mirror_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
