/* native_call_min - the smallest possible host-call exercise.
 *
 * Issues one wasmos_debug_mark (a host call that takes an immediate and
 * produces a trace record, useful for bisecting where a guest stops) and one
 * console write, then exits 0. It is the control case for
 * native_call_smoke: if this reaches its marker, the guest-to-host call path
 * works and any failure there is in the IPC layer, not in host calls.
 *
 * No preconditions: no service lookup, no IPC, either runtime. */
#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos_driver_abi.h"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    (void)wasmos_debug_mark(0x2201);

    static const char msg[] = "native-call-min: reached\n";
    (void)putsn(msg, sizeof(msg) - 1);

    return 0;
}
