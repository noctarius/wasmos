#include <stddef.h>
#include <stdint.h>
#include "boot.h"
#include "serial.h"
#include "wasmos_app.h"
#include "wasm3.h"

static void
probe_write_hex64(uint64_t value)
{
    char buf[21];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n';
    buf[19] = '\0';
    serial_write(buf);
}

static void
probe_write_bytes(const char *ptr, uint32_t len)
{
    if (!ptr || len == 0) {
        return;
    }
    char buf[128];
    uint32_t offset = 0;
    while (offset < len) {
        uint32_t chunk = len - offset;
        if (chunk > (uint32_t)(sizeof(buf) - 1)) {
            chunk = (uint32_t)(sizeof(buf) - 1);
        }
        for (uint32_t i = 0; i < chunk; ++i) {
            buf[i] = ptr[offset + i];
        }
        buf[chunk] = '\0';
        serial_write(buf);
        offset += chunk;
    }
}

m3ApiRawFunction(wasmos_console_write)
{
    (void)runtime;
    (void)_ctx;
    (void)_mem;
    m3ApiReturnType(uint32_t)
    m3ApiGetArgMem(const char *, ptr)
    m3ApiGetArg(uint32_t, len)

    if (!ptr || len == 0) {
        m3ApiReturn(0);
    }

    m3ApiCheckMem(ptr, len);
    probe_write_bytes(ptr, len);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_debug_mark)
{
    (void)runtime;
    (void)_ctx;
    (void)_mem;
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(uint32_t, tag)

    serial_write("[wasm3] debug_mark tag=");
    probe_write_hex64((uint64_t)tag);
    m3ApiReturn(0);
}

static const boot_module_t *
probe_module_at(const boot_info_t *info, uint32_t index)
{
    if (!info || !(info->flags & BOOT_INFO_FLAG_MODULES_PRESENT)) {
        return NULL;
    }
    if (!info->modules || info->module_entry_size < sizeof(boot_module_t)) {
        return NULL;
    }
    if (index >= info->module_count) {
        return NULL;
    }
    const uint8_t *mods = (const uint8_t *)info->modules;
    return (const boot_module_t *)(mods + index * info->module_entry_size);
}

int
wasm3_probe_run(const boot_info_t *info, uint32_t module_index)
{
    const boot_module_t *mod = probe_module_at(info, module_index);
    if (!mod || mod->type != BOOT_MODULE_TYPE_WASMOS_APP || mod->base == 0 ||
        mod->size == 0 || mod->size > 0xFFFFFFFFULL) {
        serial_write("[wasm3] invalid module\n");
        return -1;
    }

    wasmos_app_desc_t desc;
    if (wasmos_app_parse((const uint8_t *)(uintptr_t)mod->base,
                         (uint32_t)mod->size,
                         &desc) != 0) {
        serial_write("[wasm3] parse failed\n");
        return -1;
    }

    IM3Environment env = m3_NewEnvironment();
    if (!env) {
        serial_write("[wasm3] env alloc failed\n");
        return -1;
    }

    IM3Runtime runtime = m3_NewRuntime(env, 64 * 1024, NULL);
    if (!runtime) {
        serial_write("[wasm3] runtime alloc failed\n");
        m3_FreeEnvironment(env);
        return -1;
    }

    IM3Module module = NULL;
    M3Result res = m3_ParseModule(env, &module, desc.wasm_bytes, desc.wasm_size);
    if (res) {
        serial_write("[wasm3] parse module failed: ");
        serial_write(res);
        serial_write("\n");
        m3_FreeRuntime(runtime);
        m3_FreeEnvironment(env);
        return -1;
    }

    res = m3_LoadModule(runtime, module);
    if (res) {
        serial_write("[wasm3] load module failed: ");
        serial_write(res);
        serial_write("\n");
        m3_FreeRuntime(runtime);
        m3_FreeEnvironment(env);
        return -1;
    }

    res = m3_LinkRawFunction(module, "wasmos", "console_write", "i(ii)", &wasmos_console_write);
    if (res) {
        serial_write("[wasm3] link console_write failed: ");
        serial_write(res);
        serial_write("\n");
    }

    res = m3_LinkRawFunction(module, "wasmos", "debug_mark", "i(i)", &wasmos_debug_mark);
    if (res) {
        serial_write("[wasm3] link debug_mark failed: ");
        serial_write(res);
        serial_write("\n");
    }

    IM3Function func = NULL;
    res = m3_FindFunction(&func, runtime, "main");
    if (res) {
        serial_write("[wasm3] find main failed: ");
        serial_write(res);
        serial_write("\n");
        m3_FreeRuntime(runtime);
        m3_FreeEnvironment(env);
        return -1;
    }

    uint32_t a0 = 0;
    uint32_t a1 = 0;
    uint32_t a2 = 0;
    uint32_t a3 = 0;
    const void *args[4] = { &a0, &a1, &a2, &a3 };
    res = m3_Call(func, 4, args);
    if (res) {
        serial_write("[wasm3] call failed: ");
        serial_write(res);
        serial_write("\n");
        M3ErrorInfo info;
        m3_GetErrorInfo(runtime, &info);
        if (info.message) {
            serial_write("[wasm3] error message: ");
            serial_write(info.message);
            serial_write("\n");
        }
    } else {
        serial_write("[wasm3] call ok\n");
    }

    m3_FreeRuntime(runtime);
    m3_FreeEnvironment(env);
    return res ? -1 : 0;
}
