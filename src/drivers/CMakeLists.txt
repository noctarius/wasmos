add_subdirectory(chardev)
add_subdirectory(ata)
add_subdirectory(fs_fat)
add_subdirectory(fs_wfs)
add_subdirectory(fs_init)
add_subdirectory(serial)
add_subdirectory(keyboard)
add_subdirectory(mouse)
add_subdirectory(rtc)
add_subdirectory(virtio_net)
add_subdirectory(virtio_serial)
add_subdirectory(virtio_rng)
add_subdirectory(virtio_blk)
add_subdirectory(framebuffer)
add_subdirectory(framebuffer_pci)

add_custom_target(drivers DEPENDS
  ata_app
  fs_fat_app
  fs_wfs_app
  fs_init_app
  serial_app
  keyboard_app
  mouse_app
  rtc_app
  virtio_net_app
  virtio_serial_app
  virtio_rng_app
  framebuffer_app
  framebuffer_pci_app
  chardev_server_app
)

# The virtio-blk driver is Zig, and wasmos_add_zig_wasm_app creates no target
# when zig is absent, so it is attached only once the probe in its own
# CMakeLists has found one.
if (VIRTIO_BLK_ZIG_AVAILABLE)
  add_dependencies(drivers virtio_blk_app)
endif ()
