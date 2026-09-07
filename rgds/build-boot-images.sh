#!/bin/bash
#
# build-boot-images.sh - build the RG DS kernel and emit two flashable boot images
# entirely from source, on the host (no device, no root required).
#
#   out/boot_noc.img   stock device tree -> CPU 1992 MHz, GPU 800 MHz  (no overclock)
#   out/boot_oc.img    OC+UV device tree -> CPU 2160 MHz, GPU 900 MHz  (overclock + undervolt)
#
# Both images carry the SAME freshly built kernel and the SAME ramdisk; the only
# difference is the device tree. The device tree is COMPILED FROM SOURCE in this
# repo:
#
#   arch/arm64/boot/dts/rockchip/rk3566-anbernic-rg-ds.dts      (stock, 1992/800)
#   arch/arm64/boot/dts/rockchip/rk3566-anbernic-rg-ds-oc.dts   (overclock, 2160/900)
#
# There are NO prebuilt device-tree blobs in this repo. Inspect and edit the .dts
# above and this script recompiles them. See RG_DS_OVERCLOCK.md for the OC delta.
#
# The RG DS keeps the device tree the kernel actually boots inside the Android
# boot image's "second" area, which is a Rockchip RSCE resource image whose
# rk-kernel.dtb entry is the runtime DTB (it also carries the boot logo and
# battery bitmaps). This script rebuilds that RSCE with the freshly compiled DTB
# and repacks the boot image around it.
#
# The overclock is entirely a device-tree change (2160 MHz CPU OPP + 900 MHz GPU
# OPP + undervolt). The shipped u-boot / ATF PLL tables already support 2160 (the
# u-boot partition is byte-identical on stock and overclocked units), so no
# separate u-boot flash is needed to toggle the overclock - just flash the boot
# image you want.
#
# Usage:
#   rgds/build-boot-images.sh [stock-boot.img]
#
# The single argument is a stock RG DS boot image, used ONLY as the source of the
# ramdisk, the boot logo / battery bitmaps, and the boot-header parameters (the
# kernel and the device tree are always rebuilt from this repo, never taken from
# it). Dump it from your device once with:
#
#   adb pull /dev/block/by-name/boot stock-boot.img        # (root shell / recovery)
#
# or use the boot.img from a GammaOS release for this device. If omitted, the
# script looks for ./stock-boot.img then rgds/stock-boot.img.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KDIR="$(cd "$HERE/.." && pwd)"                 # kernel tree root
TOOLS="$HERE/tools"
OUT="$KDIR/out"
WORK="$OUT/bootbuild"
DTS_DIR="arch/arm64/boot/dts/rockchip"

# Toolchain: default to the clang used to build this tree; override with $CLANGBIN.
CLANGBIN="${CLANGBIN:-/work/GammaOSNextDistribution-A14/prebuilts/clang/host/linux-x86/clang-r487747c/bin}"
CROSS="${CROSS_COMPILE:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"

MKBOOT="$TOOLS/mkbootimg.py"
UNPACK="$TOOLS/unpack_bootimg.py"
RESTOOL="$TOOLS/resource_tool"

# variant name -> dtb basename (compiled from the matching .dts)
STOCK_DTB="rk3566-anbernic-rg-ds"
OC_DTB="rk3566-anbernic-rg-ds-oc"

log() { printf '>> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# ---- locate the template boot image ----------------------------------------
TEMPLATE="${1:-}"
if [ -z "$TEMPLATE" ]; then
  for c in "$PWD/stock-boot.img" "$HERE/stock-boot.img"; do
    [ -f "$c" ] && { TEMPLATE="$c"; break; }
  done
fi
[ -n "$TEMPLATE" ] || die "no template boot image given (see usage at top of this script)"
[ -f "$TEMPLATE" ] || die "template boot image not found: $TEMPLATE"

# ---- sanity ----------------------------------------------------------------
[ -x "$CLANGBIN/clang" ]   || die "clang not found at CLANGBIN=$CLANGBIN"
[ -f "$KDIR/.config" ]     || die "kernel not configured (no .config); run: make ARCH=arm64 rockchip_rgds_defconfig"
[ -x "$RESTOOL" ]          || die "resource_tool not found/executable at $RESTOOL"
command -v python3 >/dev/null || die "python3 required"

export PATH="$CLANGBIN:$PATH"
MAKE=(make -C "$KDIR" ARCH=arm64 LLVM="$CLANGBIN/" LLVM_IAS=1 CROSS_COMPILE="$CROSS" -j"$JOBS")

mkdir -p "$OUT"
rm -rf "$WORK"; mkdir -p "$WORK"

# ---- 1. build the kernel Image and both device trees from source -----------
log "building kernel Image (#$(cat "$KDIR/.version" 2>/dev/null || echo '?'))"
"${MAKE[@]}" Image
log "compiling device trees from source: $STOCK_DTB.dts and $OC_DTB.dts"
"${MAKE[@]}" "rockchip/$STOCK_DTB.dtb" "rockchip/$OC_DTB.dtb"

IMG="$KDIR/arch/arm64/boot/Image"
DTB_STOCK="$KDIR/$DTS_DIR/$STOCK_DTB.dtb"
DTB_OC="$KDIR/$DTS_DIR/$OC_DTB.dtb"
[ -f "$IMG" ]       || die "kernel Image not produced"
[ -f "$DTB_STOCK" ] || die "stock dtb not produced"
[ -f "$DTB_OC" ]    || die "oc dtb not produced"
log "Image: $(stat -c%s "$IMG") bytes; dtb stock: $(stat -c%s "$DTB_STOCK") oc: $(stat -c%s "$DTB_OC")"

# ---- 2. unpack the template for ramdisk + bitmaps + header params -----------
log "unpacking template boot image: $TEMPLATE"
ARGS_LINE="$(python3 "$UNPACK" --boot_img "$TEMPLATE" --out "$WORK/tmpl" --format=mkbootimg)"
[ -f "$WORK/tmpl/ramdisk" ] || die "template has no ramdisk (is this an RG DS boot image?)"
[ -f "$WORK/tmpl/second" ]  || die "template has no second/RSCE area (is this an RG DS boot image?)"

# pull the boot-header parameters (everything except the payload paths, which we override)
read_arg() { sed -n "s/.*--$1 \\([^ ]*\\).*/\\1/p" <<<"$ARGS_LINE"; }
HDRV="$(read_arg header_version)"; OSVER="$(read_arg os_version)"; OSPATCH="$(read_arg os_patch_level)"
PAGESZ="$(read_arg pagesize)"; BASE="$(read_arg base)"; KOFF="$(read_arg kernel_offset)"
ROFF="$(read_arg ramdisk_offset)"; SOFF="$(read_arg second_offset)"; TOFF="$(read_arg tags_offset)"
DOFF="$(read_arg dtb_offset)"
CMDLINE="$(sed -n "s/.*--cmdline '\\([^']*\\)'.*/\\1/p" <<<"$ARGS_LINE")"
[ -n "$PAGESZ" ] && [ -n "$CMDLINE" ] || die "could not parse boot-header parameters from template"

# extract the RSCE bitmaps (logo + battery); the runtime DTB is replaced from source
log "extracting boot logo + battery bitmaps from template RSCE"
mkdir -p "$WORK/rsce_in"
( cd "$WORK/rsce_in" && "$RESTOOL" --unpack --image="$WORK/tmpl/second" >/dev/null )
BMPDIR="$WORK/rsce_in/out"
[ -d "$BMPDIR" ] || die "resource_tool did not unpack the RSCE"
# stock RSCE entry order: rk-kernel.dtb first, then the bitmaps in this order
BITMAPS=(battery_0.bmp battery_1.bmp battery_2.bmp battery_3.bmp battery_4.bmp battery_5.bmp battery_fail.bmp logo.bmp logo_kernel.bmp)
for b in "${BITMAPS[@]}"; do
  [ -f "$BMPDIR/$b" ] || die "template RSCE missing $b"
done

# ---- 3. assemble each variant ----------------------------------------------
build_variant() {          # <name> <dtb-path>
  local name="$1" dtb="$2"
  local vdir="$WORK/$name"
  mkdir -p "$vdir/rsce"
  # rebuild the RSCE: our source-built dtb as rk-kernel.dtb (must be first), then bitmaps
  cp "$dtb" "$vdir/rsce/rk-kernel.dtb"
  cp "${BITMAPS[@]/#/$BMPDIR/}" "$vdir/rsce/"
  ( cd "$vdir/rsce" && "$RESTOOL" --pack --root="$vdir/rsce" --image="$vdir/second" \
      rk-kernel.dtb "${BITMAPS[@]}" >/dev/null )
  # repack the boot image: fresh kernel + source dtb (header dtb section) + rebuilt RSCE + template ramdisk
  python3 "$MKBOOT" \
    --header_version "$HDRV" --os_version "$OSVER" --os_patch_level "$OSPATCH" \
    --kernel "$IMG" --ramdisk "$WORK/tmpl/ramdisk" --second "$vdir/second" --dtb "$dtb" \
    --pagesize "$PAGESZ" --base "$BASE" --kernel_offset "$KOFF" --ramdisk_offset "$ROFF" \
    --second_offset "$SOFF" --tags_offset "$TOFF" --dtb_offset "$DOFF" --board '' \
    --cmdline "$CMDLINE" -o "$OUT/boot_$name.img"

  # Fix up the boot-header id, then validate.
  #
  # Rockchip u-boot (CONFIG_ANDROID_BOOT_IMAGE_HASH) recomputes the boot-header
  # SHA1 id over the payloads and refuses to boot if it does not match hdr->id:
  #   id = SHA1( kernel|kernel_size | ramdisk|ramdisk_size | second|second_size
  #              | recovery_dtbo|recovery_dtbo_size (hdr v>0)
  #              | dtb|dtb_size (hdr v>1) )
  # where an absent section contributes only its 4-byte little-endian size (0).
  # mkbootimg computes hdr->id differently, so its images are rejected by an
  # AVB-enforcing u-boot. Recompute the id the u-boot way and patch it in (this
  # is exactly what android_boot_image_editor's hashFileAndSize does), so the
  # image boots on a stock/AVB-on u-boot as well as our AVB-disabled one.
  python3 - "$OUT/boot_$name.img" "$dtb" "$UNPACK" "$RESTOOL" "$vdir" <<'PY'
import sys, subprocess, os, struct, hashlib, filecmp
img, dtb, unpack, restool, vdir = sys.argv[1:6]

chk = os.path.join(vdir, "check")
subprocess.run([sys.executable, unpack, "--boot_img", img, "--out", chk],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def sec(name):
    p = os.path.join(chk, name)
    return p if os.path.exists(p) and os.path.getsize(p) > 0 else None

# order per u-boot / android boot image v2: kernel, ramdisk, second, recovery_dtbo, dtb
hv = struct.unpack('<I', open(img, 'rb').read()[40:44])[0]
items = [sec('kernel'), sec('ramdisk'), sec('second')]
if hv > 0:
    items.append(sec('recovery_dtbo'))   # absent on this boot image -> size 0
if hv > 1:
    items.append(sec('dtb'))
md = hashlib.sha1()
for it in items:
    if it is None:
        md.update(struct.pack('<I', 0))
    else:
        data = open(it, 'rb').read()
        md.update(data); md.update(struct.pack('<I', len(data)))
new_id = md.digest()

# patch hdr->id (offset 576, 20 bytes; the 8*u32 id field, remaining bytes zero)
buf = bytearray(open(img, 'rb').read())
buf[576:576+20] = new_id
buf[576+20:576+32] = b'\x00' * 12
open(img, 'wb').write(buf)

# validate: id present + consistent, and RSCE round-trips to the source dtb
assert any(new_id), "recomputed boot id is all-zero"
rc = os.path.join(chk, "rsce"); os.makedirs(rc, exist_ok=True)
subprocess.run([restool, "--unpack", "--image=" + os.path.join(chk, "second")],
               cwd=rc, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
got = os.path.join(rc, "out", "rk-kernel.dtb")
assert filecmp.cmp(got, dtb, shallow=False), "packed RSCE rk-kernel.dtb != source dtb"
print("   validated: boot id patched (%s...), RSCE rk-kernel.dtb == source dtb" % new_id.hex()[:10])
PY
  log "out/boot_$name.img  ($(stat -c%s "$OUT/boot_$name.img") bytes)"
}

log "assembling boot images (host-side, no device needed)"
build_variant noc "$DTB_STOCK"
build_variant oc  "$DTB_OC"

cat <<EOF

Done. Two boot images built entirely from source in $OUT :

   no overclock : out/boot_noc.img   (CPU 1992 / GPU 800)   [$STOCK_DTB.dts]
   overclock    : out/boot_oc.img    (CPU 2160 / GPU 900+UV) [$OC_DTB.dts]

Flash whichever you want to the boot partition, e.g. in fastbootd:

   fastboot flash boot out/boot_oc.img

u-boot is common to both (already 2160-capable), so no separate u-boot flash is
needed to toggle the overclock.
EOF
