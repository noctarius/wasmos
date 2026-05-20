#include "wasmos/startup.h"
#include "wasmos/imports.h"

static int32_t g_wasmos_startup_args[4];
static char *g_wasmos_argv[1];

int32_t
wasmos_startup_arg(uint32_t index)
{
    if (index >= 4u) {
        return 0;
    }
    return g_wasmos_startup_args[index];
}

extern int main(int argc, char **argv);

WASMOS_WASM_EXPORT int32_t
wasmos_main(int32_t arg0,
            int32_t arg1,
            int32_t arg2,
            int32_t arg3)
{
    g_wasmos_startup_args[0] = arg0;
    g_wasmos_startup_args[1] = arg1;
    g_wasmos_startup_args[2] = arg2;
    g_wasmos_startup_args[3] = arg3;
    g_wasmos_argv[0] = 0;
    return main(0, g_wasmos_argv);
}
