add_subdirectory(sysinit)
add_subdirectory(cli)
add_subdirectory(hw_discovery)

add_custom_target(services DEPENDS
  sysinit_app
  cli_app
  hw_discovery_app
)
