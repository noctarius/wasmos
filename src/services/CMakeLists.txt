add_subdirectory(sysinit)
add_subdirectory(cli)
add_subdirectory(device_manager)
add_subdirectory(pci_bus)
add_subdirectory(acpi_bus)
add_subdirectory(fs_manager)
add_subdirectory(vt)
add_subdirectory(gfx_compositor)
add_subdirectory(font_service)
add_subdirectory(net_stack)
add_subdirectory(wasmos_script)
add_subdirectory(wasmos_script_broker)
add_subdirectory(volume_manager)

add_custom_target(services DEPENDS
  sysinit_app
  cli_app
  device_manager_app
  pci_bus_app
  acpi_bus_app
  fs_manager_app
  vt_app
  gfx_compositor_app
  font_service_app
  net_stack_app
  wamos_script_app
  wamos_script_broker_app
)

# The volume manager is Zig, so its target only exists when a toolchain was
# found. Attached here rather than listed above for the same reason the
# partition manager is: DEPENDS on a target that was never created fails the
# configure outright.
if (VOLUME_MANAGER_ZIG_AVAILABLE)
  add_dependencies(services volume_manager_app)
endif ()
