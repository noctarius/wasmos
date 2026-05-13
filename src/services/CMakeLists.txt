add_subdirectory(sysinit)
add_subdirectory(cli)
add_subdirectory(device_manager)
add_subdirectory(vt)

add_custom_target(services DEPENDS
  sysinit_app
  cli_app
  device_manager_app
  vt_app
)
