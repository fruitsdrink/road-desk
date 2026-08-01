add_executable(viewer WIN32
  main.cpp
)

target_link_libraries(viewer PRIVATE road_desk_media_client road_desk_session)

set_target_properties(viewer PROPERTIES OUTPUT_NAME "viewer")
