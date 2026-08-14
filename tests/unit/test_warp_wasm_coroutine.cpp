/* Executes a wasm32 coroutine fixture through the host-native WARP JIT.
 *
 * WARP is compiled here for the BUILD host's ISA, not the kernel's x86_64
 * target as warp_aot does, so the JIT that runs is the same compiler the kernel
 * uses but emitting code for this machine. Nothing else of WASMOS is linked:
 * there is no kernel, no process and no host-call table, which is why the
 * fixture must import nothing.
 */
#include <cstdint>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#include "src/WasmModule/WasmModule.hpp"
#include "src/core/common/NativeSymbol.hpp"
#include "src/core/common/Span.hpp"
#include "src/core/common/ILogger.hpp"

/* argv[1] is the fixture's .wasm path. The module is instantiated with an empty
 * native-symbol table and a 64 KiB guest stack owned by this frame, then its
 * "wasmos_coroutine_wasm_test" export is called with one result slot.
 *
 * Exit status: 0 when the export returns 0; 1 when it returns anything else or
 * WARP throws (a load, validation or JIT failure surfaces as an exception); 2
 * for a usage error or an unreadable/empty fixture, which distinguishes "the
 * test could not be run" from "the test failed". */
int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s fixture.wasm\n", argv[0]);
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        std::fprintf(stderr, "empty wasm fixture: %s\n", argv[1]);
        return 2;
    }
    try {
        vb::ILogger logger;
        std::vector<uint8_t> stack(64u * 1024u);
        vb::WasmModule::initEnvironment(std::malloc, std::realloc, std::free);
        {
            vb::WasmModule module(logger);
            module.initFromBytecode(vb::Span<uint8_t const>(bytes.data(), bytes.size()),
                                    vb::Span<vb::NativeSymbol const>(), false);
            module.start(stack.data() + stack.size());
            const auto result = module.callExportedFunctionWithName<1>(
                stack.data() + stack.size(), "wasmos_coroutine_wasm_test");
            if (result[0].i32 != 0) {
                std::fprintf(stderr, "WARP wasm coroutine fixture returned %d\n", result[0].i32);
                vb::WasmModule::destroyEnvironment();
                return 1;
            }
        }
        vb::WasmModule::destroyEnvironment();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "WARP wasm coroutine fixture failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
