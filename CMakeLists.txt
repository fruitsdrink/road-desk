cmake_minimum_required(VERSION 3.20)
project(road-desk VERSION 0.1.0 LANGUAGES C CXX)

# MVP: MSVC + CMake, x64 (VS 2022 or newer with C++ desktop workload).
# Prefer the CMake that ships with Visual Studio (mingw cmake may not see VS 18+).
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#   # or, from a VS Developer shell with Ninja:
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "road-desk MVP targets Windows x64 only")
endif()

# Match spike_host (/MD). Win7 Host already runs with the same CRT DLLs.
option(ROAD_DESK_STATIC_CRT "Statically link the MSVC CRT" OFF)
# Full mux media plane (default). OFF builds stubs only.
option(ROAD_DESK_WITH_MEDIA "Mux media plane + Mirror/GDI (OFF = stub)" ON)
# Legacy name kept so old scripts that pass -DROAD_DESK_WITH_LIBVNC=OFF still stub.
if(DEFINED ROAD_DESK_WITH_LIBVNC AND NOT ROAD_DESK_WITH_LIBVNC)
  set(ROAD_DESK_WITH_MEDIA OFF CACHE BOOL "" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
  # Avoid VCRUNTIME140_1.dll (FH4) — Win7 Hosts with older VC++ redist often lack it;
  # spike_host does not depend on it and runs on the same machines.
  add_compile_options(/d2FH4-)
  if(ROAD_DESK_STATIC_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>")
  endif()
endif()

add_subdirectory(src/session)
enable_testing()
add_subdirectory(src/media)
add_subdirectory(src/agent)
add_subdirectory(src/viewer)
