cmake_minimum_required(VERSION 3.20)
project(RoadDeskMirrorSetup LANGUAGES CXX RC)

# ROAD_DESK_PAYLOAD_RC = absolute path to generated payload.rc (embeds signed drivers).
if(NOT ROAD_DESK_PAYLOAD_RC OR NOT EXISTS "${ROAD_DESK_PAYLOAD_RC}")
  message(FATAL_ERROR
    "ROAD_DESK_PAYLOAD_RC missing. Run tools/mirror-install/build-installer.ps1 "
    "(after build-and-sign) so payload.rc points at signed driver files.")
endif()

add_executable(RoadDeskMirrorSetup
  main.cpp
  version.rc
  "${ROAD_DESK_PAYLOAD_RC}"
)

set_target_properties(RoadDeskMirrorSetup PROPERTIES
  OUTPUT_NAME "RoadDeskMirrorSetup"
)

target_include_directories(RoadDeskMirrorSetup PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(RoadDeskMirrorSetup PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
  _WIN32_WINNT=0x0601
  WINVER=0x0601
)

if(MSVC)
  target_compile_options(RoadDeskMirrorSetup PRIVATE /W4 /permissive- /utf-8)
  target_link_options(RoadDeskMirrorSetup PRIVATE
    "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
  )
endif()

target_link_libraries(RoadDeskMirrorSetup PRIVATE
  setupapi
  newdev
  advapi32
  shell32
  user32
)
