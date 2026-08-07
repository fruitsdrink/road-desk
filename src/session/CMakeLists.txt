add_library(road_desk_session STATIC
  auth.cpp
  session_mutex.cpp
)

target_include_directories(road_desk_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Unit tests (no gtest; same lightweight pattern as capture_strategy_test).
enable_testing()
add_executable(auth_test
  tests/auth_test.cpp
  auth.cpp
)
target_include_directories(auth_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME auth_test COMMAND auth_test)

add_executable(session_mutex_test
  tests/session_mutex_test.cpp
  session_mutex.cpp
)
target_include_directories(session_mutex_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME session_mutex_test COMMAND session_mutex_test)
