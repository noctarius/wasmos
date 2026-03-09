set(BOOT_SOURCES
  ${BOOT_DIR}/boot.c
)

add_library(bootloader_sources INTERFACE)
target_sources(bootloader_sources INTERFACE ${BOOT_SOURCES} ${BOOT_DIR}/uefi.h ${BOOT_DIR}/elf.h ${KERNEL_DIR}/include/boot.h)
target_include_directories(bootloader_sources INTERFACE ${BOOT_DIR} ${KERNEL_DIR}/include)

# IDE aggregation target for CLion indexing (not part of the build).
add_library(bootloader_ide OBJECT EXCLUDE_FROM_ALL
  ${BOOT_SOURCES}
  ${BOOT_DIR}/uefi.h
  ${BOOT_DIR}/elf.h
  ${KERNEL_DIR}/include/boot.h
)
target_include_directories(bootloader_ide PRIVATE
  ${BOOT_DIR}
  ${KERNEL_DIR}/include
)

add_custom_command(
  OUTPUT ${BUILD_DIR}/BOOTX64.EFI
  COMMAND ${CLANG} ${CFLAGS_EFI} -I${KERNEL_DIR}/include -I${BOOT_DIR}
          ${BOOT_SOURCES} -o ${BUILD_DIR}/BOOTX64.EFI ${LDFLAGS_EFI}
  DEPENDS ${BOOT_SOURCES} ${BOOT_DIR}/uefi.h ${BOOT_DIR}/elf.h ${KERNEL_DIR}/include/boot.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building UEFI bootloader"
  VERBATIM
)
add_custom_target(bootloader ALL DEPENDS ${BUILD_DIR}/BOOTX64.EFI)
