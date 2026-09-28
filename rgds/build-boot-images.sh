#!/bin/bash
#
# build-boot-images.sh - build this board's kernel and emit flashable boot images
# entirely from this tree, on the host (no device, no root, nothing outside the
# repo except the compiler).
#
# The same script is carried on every board branch of this repo; which board it
# builds comes from rgds/board.conf next to it:
#
#   BOARD      board name, for messages
#   TOOLCHAIN  clang | gcc, the compiler this board was built and verified with
#   DTBS       space separated "dtb-basename:variant" pairs, one boot image per
#              pair. An empty variant names the image out/boot.img, otherwise
#              out/boot_<variant>.img.
#
# What happens, for each variant:
#
#   1. the kernel Image is built and the board device tree is COMPILED FROM
#      SOURCE (arch/arm64/boot/dts/rockchip/<dtb-basename>.dts); there are no
#      prebuilt device-tree blobs in this repo;
#   2. the ramdisk (rgds/ramdisk.cpio.gz) and the boot-header parameters
#      (rgds/boot-header.conf) are the ones of the GammaOS boot image for this
#      board, tracked in the tree; a boot image passed as the first argument
#      overrides both (the kernel and device tree are never taken from it);
#   3. the Rockchip RSCE resource ("second" area, which u-boot reads for the
#      runtime device tree as rk-kernel.dtb plus the boot logo and charge
#      animation) is rebuilt from the fresh DTB and the bitmaps in rgds/rsce;
#   4. the boot image is repacked and its header id recomputed the way Rockchip
#      u-boot checks it.
#
# Usage:
#   rgds/build-boot-images.sh                    # fresh clone: this is enough
#   rgds/build-boot-images.sh some-boot.img      # reuse that image's ramdisk/header
#
# If the tree has no .config yet, rockchip_rgds_defconfig is applied first.
#
# Toolchain: see rgds/toolchain.sh (CLANGBIN / CROSS_COMPILE) and rgds/README.md.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KDIR="$(cd "$HERE/.." && pwd)"                 # kernel tree root
TOOLS="$HERE/tools"
OUT="$KDIR/out"
WORK="$OUT/bootbuild"
DTS_DIR="arch/arm64/boot/dts/rockchip"
DEFCONFIG="rockchip_rgds_defconfig"

MKBOOT="$TOOLS/mkbootimg.py"
UNPACK="$TOOLS/unpack_bootimg.py"
RESTOOL="$TOOLS/resource_tool"

log() { printf '>> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# ---- board + toolchain -------------------------------------------------------
# shellcheck disable=SC1091
. "$HERE/toolchain.sh"
log "board: $BOARD  toolchain: $TOOLCHAIN_DESC"

# ---- ramdisk + boot-header source -------------------------------------------
# Default: the tracked rgds/ramdisk.cpio.gz + rgds/boot-header.conf, so a fresh
# clone builds with no arguments. A boot image passed as $1 overrides both (its
# ramdisk and header fields are used; kernel, dtb and RSCE still come from here).
TEMPLATE="${1:-}"
RAMDISK_SRC="$HERE/ramdisk.cpio.gz"
HEADER_CONF="$HERE/boot-header.conf"
if [ -n "$TEMPLATE" ]; then
  [ -f "$TEMPLATE" ] || die "template boot image not found: $TEMPLATE"
else
  [ -f "$RAMDISK_SRC" ] && [ -f "$HEADER_CONF" ] || die \
    "missing rgds/ramdisk.cpio.gz or rgds/boot-header.conf, and no template boot image given"
fi

# ---- sanity ----------------------------------------------------------------
[ -x "$RESTOOL" ]          || die "resource_tool not found/executable at $RESTOOL"
command -v python3 >/dev/null || die "python3 required"
[ -f "$KDIR/arch/arm64/configs/$DEFCONFIG" ] || die "missing arch/arm64/configs/$DEFCONFIG"

mkdir -p "$OUT"
rm -rf "$WORK"; mkdir -p "$WORK"

# ---- 0. configure on a fresh checkout ---------------------------------------
if [ ! -f "$KDIR/.config" ]; then
  log "no .config, applying $DEFCONFIG"
  "${MAKE[@]}" "$DEFCONFIG"
fi

# ---- 1. build the kernel Image and the device tree(s) from source -----------
VARIANT_NAMES=(); VARIANT_DTBS=()
for pair in $DTBS; do
  VARIANT_DTBS+=("${pair%%:*}")
  VARIANT_NAMES+=("${pair#*:}")
done
[ "${#VARIANT_DTBS[@]}" -gt 0 ] || die "board.conf DTBS is empty"

log "building kernel Image (#$(cat "$KDIR/.version" 2>/dev/null || echo '?'))"
"${MAKE[@]}" Image
log "compiling device tree(s) from source: ${VARIANT_DTBS[*]}"
DTB_TARGETS=("${VARIANT_DTBS[@]/#/rockchip/}"); DTB_TARGETS=("${DTB_TARGETS[@]/%/.dtb}")   # rockchip/<name>.dtb
"${MAKE[@]}" "${DTB_TARGETS[@]}"
IMG="$KDIR/arch/arm64/boot/Image"
[ -f "$IMG" ] || die "kernel Image not produced"
for d in "${VARIANT_DTBS[@]}"; do
  [ -f "$KDIR/$DTS_DIR/$d.dtb" ] || die "$d.dtb not produced"
done
log "Image: $(stat -c%s "$IMG") bytes"

# ---- 2. ramdisk + boot-header parameters ------------------------------------
mkdir -p "$WORK/tmpl"
if [ -n "$TEMPLATE" ]; then
  log "unpacking template boot image: $TEMPLATE"
  ARGS_LINE="$(python3 "$UNPACK" --boot_img "$TEMPLATE" --out "$WORK/tmpl" --format=mkbootimg)"
  [ -f "$WORK/tmpl/ramdisk" ] || die "template has no ramdisk (is this a boot image for the $BOARD?)"
  # pull the boot-header parameters (everything except the payload paths, which we override)
  read_arg() { sed -n "s/.*--$1 \\([^ ]*\\).*/\\1/p" <<<"$ARGS_LINE"; }
  HDRV="$(read_arg header_version)"; OSVER="$(read_arg os_version)"; OSPATCH="$(read_arg os_patch_level)"
  PAGESZ="$(read_arg pagesize)"; BASE="$(read_arg base)"; KOFF="$(read_arg kernel_offset)"
  ROFF="$(read_arg ramdisk_offset)"; SOFF="$(read_arg second_offset)"; TOFF="$(read_arg tags_offset)"
  DOFF="$(read_arg dtb_offset)"
  CMDLINE="$(sed -n "s/.*--cmdline '\\([^']*\\)'.*/\\1/p" <<<"$ARGS_LINE")"
  [ -n "$PAGESZ" ] && [ -n "$CMDLINE" ] || die "could not parse boot-header parameters from template"
else
  log "using rgds/ramdisk.cpio.gz + rgds/boot-header.conf"
  cp "$RAMDISK_SRC" "$WORK/tmpl/ramdisk"
  # shellcheck disable=SC1090
  . "$HEADER_CONF"
  for v in HDRV OSVER OSPATCH PAGESZ BASE KOFF ROFF SOFF TOFF DOFF CMDLINE; do
    [ -n "${!v:-}" ] || die "rgds/boot-header.conf does not set $v"
  done
fi

# The RSCE bitmaps (boot logo + charge animation) come from rgds/rsce in this
# tree, so the boot image is buildable from source alone. Falling back to the
# template keeps older checkouts working.
# Stock RSCE entry order: rk-kernel.dtb first, then the bitmaps in this order.
BITMAPS=(battery_0.bmp battery_1.bmp battery_2.bmp battery_3.bmp battery_4.bmp battery_5.bmp battery_fail.bmp logo.bmp logo_kernel.bmp)
BMPDIR="$HERE/rsce"
if [ -d "$BMPDIR" ]; then
  log "using boot logo + battery bitmaps from rgds/rsce"
elif [ -n "$TEMPLATE" ] && [ -f "$WORK/tmpl/second" ]; then
  log "rgds/rsce missing, falling back to the template RSCE"
  mkdir -p "$WORK/rsce_in"
  ( cd "$WORK/rsce_in" && "$RESTOOL" --unpack --image="$WORK/tmpl/second" >/dev/null )
  BMPDIR="$WORK/rsce_in/out"
  [ -d "$BMPDIR" ] || die "resource_tool did not unpack the RSCE"
else
  die "rgds/rsce is missing and there is no template boot image to take the bitmaps from"
fi
for b in "${BITMAPS[@]}"; do
  [ -f "$BMPDIR/$b" ] || die "missing RSCE bitmap $b in $BMPDIR"
done

# ---- 3. assemble each variant ----------------------------------------------
build_variant() {          # <variant-name or empty> <dtb-path>
  local name="$1" dtb="$2"
  local img_name="boot${name:+_$name}.img"
  local vdir="$WORK/${name:-boot}"
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
    --cmdline "$CMDLINE" -o "$OUT/$img_name"

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
  python3 - "$OUT/$img_name" "$dtb" "$UNPACK" "$RESTOOL" "$vdir" <<'PY'
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
    items.append(sec('recovery_dtbo'))   # absent on these boot images -> size 0
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
  log "out/$img_name  ($(stat -c%s "$OUT/$img_name") bytes)  [$(basename "$dtb" .dtb).dts]"
}

log "assembling boot image(s) (host-side, no device needed)"
for i in "${!VARIANT_DTBS[@]}"; do
  build_variant "${VARIANT_NAMES[$i]}" "$KDIR/$DTS_DIR/${VARIANT_DTBS[$i]}.dtb"
done

cat <<EOF

Done. Boot image(s) for the $BOARD built entirely from source in $OUT.
Flash to the boot partition, e.g. in fastbootd:  fastboot flash boot out/<image>

The WiFi module must come from the same tree and compiler as this kernel:
   rgds/build-wifi.sh        -> out/8821cs.ko  (goes in the vendor image)
EOF

if printf '%s\n' "${VARIANT_NAMES[@]}" | grep -qx oc; then
  cat <<'EOF'

Overclock image: the 2160 MHz CPU point ALSO needs a bootloader whose ATF
carries that PLL rate. Stock rkbin BL31 does not: with it the kernel reports
2160 while the hardware stays at 1992, vdd_cpu never leaves the 1992 voltage,
and a benchmark shows no gain. Anbernic's shipped u-boot has the entry; a
from-source build needs oc/atf-2160-patch.py from the rg_ds_uboot tree. Check
any image with:
   dumpimage -T flat_dt -p 3 -o s3.bin uboot.img   # u32 at 0x178:
   2160000000 = OC-capable, 312000000 = not.
EOF
fi
