add_executable(host-agent
  main.cpp
  log.cpp
)

target_link_libraries(host-agent PRIVATE road_desk_media_host road_desk_session)

set_target_properties(host-agent PROPERTIES OUTPUT_NAME "host-agent")
