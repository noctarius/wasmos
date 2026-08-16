#!/usr/bin/env bash
# Install a specific LLVM release from apt.llvm.org, retrying transient
# failures.
#
# Ubuntu's own clang mis-handles the x86_64-unknown-uefi target (bootloader
# codegen: "backend data layout 'e-m:w-...' does not match expected target
# description 'e-m:e-...'"), so the workflows install upstream LLVM rather than
# the distro package.
#
# The retry is not defensive habit. llvm.sh decides whether it supports the
# running distribution by making a HEAD request to
# https://apt.llvm.org/<codename>/, and treats ANY failure of that request as
# "unsupported distribution", exits 2, and prints a message naming the Ubuntu
# version -- which reads like the runner image moved out from under us. It has
# not: in CI run 31947884623 exactly one job of thirteen died that way while the
# other twelve installed the same LLVM on the same image. A third-party host
# being briefly unreachable should cost a retry, not a red run.
set -euo pipefail

version="${1:?usage: install-llvm.sh <llvm-major-version>}"
attempts="${LLVM_INSTALL_ATTEMPTS:-3}"

for attempt in $(seq 1 "$attempts"); do
    if curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh &&
        sudo bash /tmp/llvm.sh "$version" all; then
        exit 0
    fi
    if [[ "$attempt" -lt "$attempts" ]]; then
        delay=$((attempt * 10))
        echo "install-llvm: attempt ${attempt}/${attempts} failed; retrying in ${delay}s" >&2
        sleep "$delay"
    fi
done

echo "install-llvm: LLVM ${version} could not be installed after ${attempts} attempts" >&2
exit 1
