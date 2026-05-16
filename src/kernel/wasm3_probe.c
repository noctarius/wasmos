#include <stddef.h>
#include "klog.h"
#include <stdint.h>
#include "boot.h"
#include "wasmos_app.h"
#include "wasm3.h"
#include "wasm3_link.h"
#include "process.h"
#include "wasm3_shim.h"

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
    char entry_name[64];
    uint32_t previous_pid = wasm3_heap_bind_pid(process_current_pid());
    preempt_disable();
    wasm3_heap_configure(process_current_pid(), 0, 2ULL * 1024ULL * 1024ULL * 1024ULL);
    if (wasm3_heap_probe_growth(6u * 1024u * 1024u) != 0) {
        klog_write("[wasm3] heap growth probe failed\n");
        preempt_enable();
        wasm3_heap_restore_pid(previous_pid);
        return -1;
    }
    klog_write("[test] wasm heap grow ok\n");
    const boot_module_t *mod = probe_module_at(info, module_index);
    if (!mod || mod->type != BOOT_MODULE_TYPE_WASMOS_APP || mod->base == 0 ||
        mod->size == 0 || mod->size > 0xFFFFFFFFULL) {
        klog_write("[wasm3] invalid module\n");
        preempt_enable();
        wasm3_heap_restore_pid(previous_pid);
        return -1;
    }

    wasmos_app_desc_t desc;
    if (wasmos_app_parse((const uint8_t *)(uintptr_t)mod->base,
                         (uint32_t)mod->size,
                         &desc) != 0) {
        klog_write("[wasm3] parse failed\n");
        preempt_enable();
        wasm3_heap_restore_pid(previous_pid);
        return -1;
    }
    if (desc.entry_len == 0 || desc.entry_len >= sizeof(entry_name)) {
        klog_write("[wasm3] invalid entry name\n");
        preempt_enable();
        wasm3_heap_restore_pid(previous_pid);
        return -1;
    }
    for (uint32_t i = 0; i < desc.entry_len; ++i) {
        entry_name[i] = (char)desc.entry[i];
    }
    entry_name[desc.entry_len] = '\0';

    IM3Environment env = m3_NewEnvironment();
    if (!env) {
        klog_write("[wasm3] env alloc failed\n");
        preempt_enable();
        wasm3_heap_restore_pid(previous_pid);
        return -1;
    }

    IM3Runtime runtime = m3_NewRuntime(env, 64 * 1024, NULL);
    if (!runtime) {
        klog_write("[wasm3] runtime alloc failed\n");
        m3_FreeEnvironment(env);
        preempt_enable();
        wasm3_heap_restore_pid(previous_pid);
        return -1;
    }

    IM3Module module = NULL;
    M3Result res = m3_ParseModule(env, &module, desc.wasm_bytes, desc.wasm_size);
    if (res) {
        klog_write("[wasm3] parse module failed: ");
        klog_write(res);
        klog_write("\n");
        m3_FreeRuntime(runtime);
        m3_FreeEnvironment(env);
        preempt_enable();
        wasm3_heap_restore_pid(previous_pid);
        return -1;
    }

    res = m3_LoadModule(runtime, module);
    if (res) {
        klog_write("[wasm3] load module failed: ");
        klog_write(res);
        klog_write("\n");
        m3_FreeRuntime(runtime);
        m3_FreeEnvironment(env);
        preempt_enable();
        wasm3_heap_restore_pid(previous_pid);
        return -1;
    }

    if (wasm3_link_wasmos(module) != 0) {
        klog_write("[wasm3] link wasmos failed\n");
    }
    if (wasm3_link_env(module) != 0) {
        klog_write("[wasm3] link env failed\n");
    }

    IM3Function func = NULL;
    res = m3_FindFunction(&func, runtime, entry_name);
    if (res) {
        klog_write("[wasm3] find entry failed: ");
        klog_write(res);
        klog_write("\n");
        m3_FreeRuntime(runtime);
        m3_FreeEnvironment(env);
        preempt_enable();
        wasm3_heap_restore_pid(previous_pid);
        return -1;
    }

    uint32_t a0 = 0;
    uint32_t a1 = 0;
    uint32_t a2 = 0;
    uint32_t a3 = 0;
    const void *args[4] = { &a0, &a1, &a2, &a3 };
    res = m3_Call(func, 4, args);
    if (res) {
        klog_write("[wasm3] call failed: ");
        klog_write(res);
        klog_write("\n");
        M3ErrorInfo info;
        m3_GetErrorInfo(runtime, &info);
        if (info.message) {
            klog_write("[wasm3] error message: ");
            klog_write(info.message);
            klog_write("\n");
        }
    } else {
        klog_write("[wasm3] call ok\n");
    }

    m3_FreeRuntime(runtime);
    m3_FreeEnvironment(env);
    preempt_enable();
    wasm3_heap_restore_pid(previous_pid);
    return res ? -1 : 0;
}
