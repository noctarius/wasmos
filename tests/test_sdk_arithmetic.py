"""What the SDK can and cannot compile arithmetically, pinned.

`docs/toolchain.md` says the SDK ships no `libclang_rt.builtins-wasm32.a`. That is
only a defensible gap if the boundary is known, so this measures it rather than
asserting the absence: WebAssembly has native i64 divide, remainder and multiply
and native float conversions, so 64-bit C arithmetic needs no compiler-rt at all,
and the only shape that does is `__int128`.

Both halves matter. The first is the claim a developer relies on -- ordinary
`uint64_t`/`double` code links -- and it would break silently if a future flag
change made clang lower those to library calls. The second pins the failure MODE
of the gap: because the C link no longer passes `--allow-undefined`, reaching
`__int128` is a link error naming the missing symbol, not a module that loads and
traps at the call site. If someone builds compiler-rt for wasm32, the second test
starts failing and should be inverted -- which is the point.
"""

import os
import re
import subprocess
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD_DIR = os.environ.get("WASMOS_BUILD_DIR", os.path.join(ROOT, "build"))
WASMOS_CLANG = os.path.join(BUILD_DIR, "wasmos-sdk", "bin", "wasmos-clang")

# Every builtin a wasm32 module can end up needing, measured by compiling each
# arithmetic shape and reading the undefined symbols. All eight are reachable only
# through __int128: the i64 and float shapes need none.
I128_BUILTINS = frozenset(
    {
        "__multi3",
        "__udivti3",
        "__divti3",
        "__umodti3",
        "__modti3",
        "__fixdfti",
        "__fixunsdfti",
        "__floatuntidf",
    }
)

SIXTY_FOUR_BIT_PROGRAM = """
#include <stdint.h>
#include <stdio.h>

volatile uint64_t ua = 123456789012345ull, ub = 987654321ull;
volatile int64_t sa = -123456789012345ll, sb = 98765ll;
volatile double d = 2.5;
volatile float f = 1.5f;

int main(void) {
    printf("%llu %llu %lld %lld %f %f\\n", (unsigned long long)(ua / ub),
           (unsigned long long)(ua % ub), (long long)(sa / sb), (long long)(sa % sb),
           (double)ua + d, (double)f);
    return 0;
}
"""

I128_PROGRAM = """
typedef unsigned __int128 u128;
volatile u128 a = (u128)1 << 100, b = 12345;
int main(void) { return (int)((a / b) * b); }
"""


@unittest.skipUnless(
    os.access(WASMOS_CLANG, os.X_OK),
    "SDK not staged (cmake --build build --target wasmos-sdk)",
)
class SdkArithmeticTest(unittest.TestCase):
    def _compile(self, source: str, name: str) -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, f"{name}.c")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(source)
            return subprocess.run(
                [WASMOS_CLANG, path, "-o", os.path.join(tmp, name)],
                capture_output=True,
                text=True,
                timeout=120,
            )

    def test_64_bit_arithmetic_needs_no_builtins(self):
        result = self._compile(SIXTY_FOUR_BIT_PROGRAM, "sixtyfour")
        self.assertEqual(
            result.returncode,
            0,
            f"64-bit arithmetic failed to link:\n{result.stderr}",
        )

    def test_int128_fails_as_a_named_link_error(self):
        result = self._compile(I128_PROGRAM, "onetwentyeight")
        self.assertNotEqual(
            result.returncode,
            0,
            "__int128 linked, so compiler-rt builtins are now present: invert this "
            "test and drop the gap from docs/toolchain.md",
        )
        missing = set(re.findall(r"undefined symbol: (__\w+)", result.stderr))
        self.assertTrue(
            missing,
            f"__int128 failed without naming a missing symbol:\n{result.stderr}",
        )
        self.assertTrue(
            missing <= I128_BUILTINS,
            f"unexpected missing builtins {sorted(missing - I128_BUILTINS)}; the "
            "measured set in this test is out of date",
        )


if __name__ == "__main__":
    unittest.main()
