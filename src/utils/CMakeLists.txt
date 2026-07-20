add_subdirectory(date)
add_subdirectory(sched_info)
add_subdirectory(ps)
add_subdirectory(cat)
add_subdirectory(sysinfo)
add_subdirectory(explorer)
add_subdirectory(ip)

# Aggregate every /system/utils tool so the run-qemu-* targets (which depend on
# `system_utils`) rebuild them before the ESP is populated. The date tool
# defines `system_utils` itself only when AssemblyScript is enabled, so create
# it here when absent, and attach each C/Zig util that was configured.
if (NOT TARGET system_utils)
  add_custom_target(system_utils)
endif ()
foreach(_util sched_info_util ps_util cat_util sysinfo_util explorer_util ip_util)
  if (TARGET ${_util})
    add_dependencies(system_utils ${_util})
  endif ()
endforeach()
