add_subdirectory(chardev)
add_subdirectory(ata)
add_subdirectory(fs_fat)
add_subdirectory(fs_init)
add_subdirectory(serial)
add_subdirectory(keyboard)
add_subdirectory(framebuffer)

add_custom_target(drivers DEPENDS
  ata_app
  fs_fat_app
  fs_init_app
  serial_app
  keyboard_app
  framebuffer_app
  chardev_driver_blob
)
