add_executable(host-agent
  main.cpp
  log.cpp
)

target_include_directories(host-agent PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(host-agent PRIVATE road_desk_media_host road_desk_session)

set_target_properties(host-agent PROPERTIES OUTPUT_NAME "host-agent")

# Mirror Attach.ToDesktop scrub needs HKLM write — always elevate via UAC.
if(MSVC)
  target_link_options(host-agent PRIVATE
    "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
  )
  # Keep a full manifest beside sources for documentation / alternate toolchains.
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/host-agent.manifest"
    "${CMAKE_CURRENT_BINARY_DIR}/host-agent.manifest"
    COPYONLY
  )
endif()
