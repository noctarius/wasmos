/// The translated libsys_native C ABI, imported once so every Zig caller sees
/// one set of types. `libsys.zig` wraps what a service should use; reach for
/// this namespace directly only for declarations that have no wrapper. The
/// translate-c build carries no libc include path, which is why the headers it
/// pulls in must not include `wasmos_cast.h`.
pub const c = @cImport({
    @cInclude("wasmos_driver_abi.h");
    @cInclude("wasmos/libsys_native.h");
    @cInclude("wasmos/coroutine_native.h");
});
