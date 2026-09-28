#!/bin/bash
#
# build-wifi.sh - build the RTL8821CS SDIO WiFi driver (external/rtl8821cs) as an
# out-of-tree module against THIS tree, with THIS board's compiler, and emit
# out/8821cs.ko for the vendor image (/vendor/lib/modules/8821cs.ko).
#
# Three things must hold or wlan0 never comes up, all of them enforced here:
#
#   1. The module is built against the same tree that produced the flashed
#      boot image, with a Module.symvers that matches that kernel. A module
#      from another tree, or from a stale Module.symvers, is rejected at load
#      with "disagrees about version of symbol module_layout". `make modules`
#      is run first so modpost regenerates Module.symvers from the current
#      vmlinux.
#   2. The same compiler as the kernel (rgds/board.conf, TOOLCHAIN).
#   3. The driver source in external/rtl8821cs is already patched to build as
#      a full cfg80211/nl80211 driver named "8821cs" (the stock name); the
#      unpatched Realtek Makefile omits cfg80211 for CONFIG_PLATFORM_ARM64_ALL
#      and wificond then cannot find a wiphy.
#
# Usage:
#   rgds/build-wifi.sh          (after rgds/build-boot-images.sh, same checkout)
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KDIR="$(cd "$HERE/.." && pwd)"
OUT="$KDIR/out"
WIFI="$KDIR/external/rtl8821cs"

log() { printf '>> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# shellcheck disable=SC1091
. "$HERE/toolchain.sh"
log "board: $BOARD  toolchain: $TOOLCHAIN_DESC"

[ -d "$WIFI" ]          || die "driver source missing at external/rtl8821cs"
[ -f "$KDIR/.config" ]  || die "kernel not configured; run rgds/build-boot-images.sh first"
[ -f "$KDIR/vmlinux" ]  || die "kernel not built (no vmlinux); run rgds/build-boot-images.sh first"
mkdir -p "$OUT"

# The external module build needs the make variables, not just the make command.
MAKE_VARS=("${MAKE[@]:3}")            # drop "make -C $KDIR"; keep ARCH=..., LLVM=..., -j
MAKE_VARS=("${MAKE_VARS[@]//-j*/}")   # jobs are passed explicitly below

# 1. Module.symvers must describe the vmlinux this tree just produced.
log "regenerating Module.symvers against $KDIR"
"${MAKE[@]}" modules >/dev/null
log "module_layout: $(grep -w module_layout "$KDIR/Module.symvers" | awk '{print $1}')"

# 2. Build the external module. The vendor source is not warning-clean.
log "building 8821cs.ko"
make -C "$WIFI" clean >/dev/null 2>&1 || true
# shellcheck disable=SC2068
make -C "$WIFI" ${MAKE_VARS[@]} KSRC="$KDIR" M="$WIFI" USER_EXTRA_CFLAGS="-Wno-error" -j"$JOBS" modules

KO="$WIFI/8821cs.ko"
[ -f "$KO" ] || die "no 8821cs.ko produced"
case "$TOOLCHAIN" in
  clang) "$CLANGBIN/llvm-strip" --strip-debug "$KO" -o "$OUT/8821cs.ko" ;;
  gcc)   "${CROSS}strip" --strip-debug "$KO" -o "$OUT/8821cs.ko" ;;
esac

MODINFO="$(command -v modinfo || true)"
log "built out/8821cs.ko ($(stat -c%s "$OUT/8821cs.ko") bytes)"
if [ -n "$MODINFO" ]; then
  log "name    : $("$MODINFO" "$OUT/8821cs.ko" | awk '/^name:/{print $2}')"
  log "vermagic: $("$MODINFO" "$OUT/8821cs.ko" | awk -F: '/^vermagic/{print $2}')"
fi
log "deploy: replace /vendor/lib/modules/8821cs.ko in the GammaOS vendor image (vendor_dlkm on the RG DS Plus)"
