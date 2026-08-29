/* wfs_status.h - the generated packed error codes, reachable from both worlds.
 *
 * The SDK sysroot does not put abi/generated/c on the include path, so a driver
 * compiled by wasmos-clang cannot find wasmos_status.h by name. The rest of the
 * tree solves this by relative path — src/drivers/include/wasmos_driver_abi.h
 * includes the same file the same way — and this does it once so every WFS
 * header can just include this.
 *
 * The path resolves against THIS file's directory, so it works whoever includes
 * it: the driver, the host formatter under src/tools, and the host test suites.
 * A build that already has -I abi/generated/c is unaffected, because the include
 * names a file rather than relying on the search path.
 */
#ifndef FS_WFS_WFS_STATUS_H
#define FS_WFS_WFS_STATUS_H

#include "../../../abi/generated/c/wasmos_status.h"

#endif /* FS_WFS_WFS_STATUS_H */
