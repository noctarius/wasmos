#!/bin/sh
# fetch-ca-certs.sh - fetch and pin the CA trust store for the WASMOS net-stack.
#
# Downloads a PINNED, dated curl.se cacert.pem (Mozilla CA bundle) and verifies
# its SHA-256 against a pinned constant. The verified bundle is written to
#   scripts/system/net/certificates/ca-certs.pem   (default OUT below)
# from where the top-level CMakeLists.txt copies it into the ESP at guest path
#   /system/net/certificates/ca-certs.pem
# and net-stack loads it as its TLS trust store (milestone C, VERIFY_REQUIRED).
#
# Reproducible / offline-friendly:
#   - If OUT already exists and its SHA-256 matches the pin, do nothing (no refetch).
#   - Otherwise download the pinned URL, verify the checksum, and install it.
#   - If no network is available and there is no valid cached copy, fail loudly.
#
# NOTE: the hermetic verification test (tests/test_net_stack_https_verify_e2e.py)
# does NOT use this real bundle: it overwrites the ESP copy with its own
# self-signed test CA. This bundle is for real-world (public CA) HTTPS.
#
# Bumping the pin: pick a newer dated file from https://curl.se/docs/caextract.html,
# download it, compute its SHA-256, and update PINNED_URL + PINNED_SHA256 together.
set -eu

# --- Pinned bundle (dated URL is immutable; the checksum guards integrity) -----
PINNED_URL="https://curl.se/ca/cacert-2026-07-16.pem"
PINNED_SHA256="3ff344e30b9b1ed2971044eabb438a08f2e2245ddb5f8ab1a3ad8b63ab4eaf91"

# Output path (overridable as $1). Default lands in the repo tree the ESP copies from.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT="${1:-$SCRIPT_DIR/system/net/certificates/ca-certs.pem}"

# --- Portable SHA-256 helper (Linux sha256sum / macOS shasum) ------------------
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        echo "fetch-ca-certs: no sha256sum/shasum available to verify the bundle" >&2
        exit 3
    fi
}

# --- Fast path: valid cached copy already present ------------------------------
if [ -f "$OUT" ]; then
    if [ "$(sha256_of "$OUT")" = "$PINNED_SHA256" ]; then
        echo "fetch-ca-certs: cached bundle up to date ($OUT)"
        exit 0
    fi
    echo "fetch-ca-certs: cached bundle checksum mismatch, refetching" >&2
fi

# --- Fetch to a temp file, verify, then install atomically --------------------
mkdir -p "$(dirname -- "$OUT")"
TMP="$OUT.tmp.$$"
trap 'rm -f "$TMP"' EXIT INT TERM

fetched=0
if command -v curl >/dev/null 2>&1; then
    if curl -fsS --max-time 60 -o "$TMP" "$PINNED_URL"; then
        fetched=1
    fi
elif command -v wget >/dev/null 2>&1; then
    if wget -q -T 60 -O "$TMP" "$PINNED_URL"; then
        fetched=1
    fi
else
    echo "fetch-ca-certs: neither curl nor wget available to download the bundle" >&2
    exit 4
fi

if [ "$fetched" -ne 1 ]; then
    echo "fetch-ca-certs: network unavailable and no valid cached copy at $OUT" >&2
    echo "               (download of $PINNED_URL failed)" >&2
    exit 5
fi

GOT=$(sha256_of "$TMP")
if [ "$GOT" != "$PINNED_SHA256" ]; then
    echo "fetch-ca-certs: SHA-256 MISMATCH for $PINNED_URL" >&2
    echo "  expected: $PINNED_SHA256" >&2
    echo "  got:      $GOT" >&2
    echo "  refusing to install an unverified CA bundle" >&2
    exit 6
fi

mv -f "$TMP" "$OUT"
trap - EXIT INT TERM
echo "fetch-ca-certs: installed verified bundle -> $OUT"
