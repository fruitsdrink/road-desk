add_library(road_desk_common STATIC
  http_helpers.cpp
)

target_include_directories(road_desk_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(road_desk_common PUBLIC
  ws2_32
  winhttp
  ole32
)
