/* priv_probe - a package that declares a privileged capability (svc.control)
 * and does nothing with it: it reports that it started and exits.
 *
 * The build packs it three ways (examples/c/BUILD.py): signed with the build's
 * key (priv_probe.wap), unsigned (priv_unsigned.wap), and signed then altered
 * (priv_tampered.wap). The kernel may start only the first, and only for a
 * requester that holds svc.control itself or holds spawn.grant
 * (tests/test_signed_packages.py). */
#include <stdint.h>

#include "string.h"
#include "wasmos/api.h"

int main(void) {
    static const char line[] = "[priv-probe] started\n";
    (void)wasmos_console_write(addr_cast(int32_t, line), (int32_t)(sizeof(line) - 1u));
    return 0;
}
