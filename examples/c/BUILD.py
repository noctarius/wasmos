"""C example guest modules."""

from wasmos_rules import kernel_blob, wasm_c_app


def build(b):
    # chardev_client is also embedded in the kernel image as a blob.
    wasm, _ = wasm_c_app(
        b,
        "chardev_client",
        [b.src("chardev_client/chardev_client.c")],
        b.src("chardev_client/linker.metadata"),
    )
    b.exports.setdefault("kernel.blobs", []).append(kernel_blob(b, wasm))
    # TODO: port the remaining examples/c modules from examples/c/CMakeLists.txt.
