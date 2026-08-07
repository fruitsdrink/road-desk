# Unit tests for agent modules (session_monitor, injection_pipe, etc.).

# session_monitor_test: tests cross-desktop GDI capture and desktop state logic.
# Links session_monitor.cpp + log.cpp (needs media_log from road_desk_media_client).
add_executable(session_monitor_test
  session_monitor_test.cpp
  ../session_monitor.cpp
  ../log.cpp
)
target_include_directories(session_monitor_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/agent
  ${CMAKE_SOURCE_DIR}/src/media
)
target_link_libraries(session_monitor_test PRIVATE
  road_desk_media_client
  wtsapi32
  gdi32
  user32
)
add_test(NAME session_monitor_test COMMAND session_monitor_test)
