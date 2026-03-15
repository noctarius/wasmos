add_subdirectory(chardev)
add_subdirectory(ata)
add_subdirectory(fs_fat)
add_subdirectory(serial)

add_custom_target(drivers DEPENDS
  ata_app
  fs_fat_app
  serial_app
  chardev_driver_blob
)
