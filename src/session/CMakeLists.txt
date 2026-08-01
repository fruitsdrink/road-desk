add_library(road_desk_session STATIC
  auth.cpp
  session_mutex.cpp
)

target_include_directories(road_desk_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
