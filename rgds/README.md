# RG DS kernel build

This is the Linux 6.1 kernel for the Anbernic RG DS (Rockchip RK3566, dual
640x480 DSI panels), built as a functional drop-in for the stock 6.1.141 kernel.
This branch (`main`) builds the RG DS; the `rgdsplus-main` branch of the same
repository builds the RG DS Plus with the same scripts and driver code, the two
differ in their board device trees, boot logos and `rgds/board.conf`.

Everything a boot image needs is in this tree: kernel, board device tree as
source, the RSCE boot logo and charge animation bitmaps, the GammaOS ramdisk and
boot header parameters, and the WiFi driver source. The only thing to install
is the compiler.

The board device tree is kept **as source**, not as a prebuilt blob:

| file | result |
|------|--------|
| `arch/arm64/boot/dts/rockchip/rk3566-anbernic-rg-ds.dts`    | stock clocks: CPU 1992 MHz, GPU 800 MHz |
| `arch/arm64/boot/dts/rockchip/rk3566-anbernic-rg-ds-oc.dts` | overclock: CPU 2160 MHz per bin + undervolt |

Both are compiled by the normal kernel `dtbs` build. The overclock delta (which
OPP/voltage lines differ) is documented in
`arch/arm64/boot/dts/rockchip/RG_DS_OVERCLOCK.md`.

## Toolchain

The RG DS kernel is built with Android clang `r487747c` (`LLVM=1`), the
toolchain of the stock kernel (`rgds/board.conf`, `TOOLCHAIN=clang`). Get it
from the AOSP prebuilts, only that one version is needed:

```
git clone --depth=1 --filter=blob:none --sparse \
    https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86 aosp-clang
git -C aosp-clang sparse-checkout set clang-r487747c
export CLANGBIN=$PWD/aosp-clang/clang-r487747c/bin
```

Without `CLANGBIN` the scripts use the `clang` on your `PATH` and warn if it is
another version. Another clang builds a working kernel, but every module loaded
into it must then come from the same build too (see WiFi module below); the
stock `8821cs.ko` will not load into it.

Also needed: `python3`, `make`, `flex`, `bison`, `bc`, `libssl-dev`,
`libelf-dev`, and a host `aarch64-linux-gnu-` binutils prefix is not required
(clang's own LLVM binutils are used).

## Build a kernel + two flashable boot images

```
rgds/build-boot-images.sh
```

That is the whole build on a fresh clone. The script:

1. applies `rockchip_rgds_defconfig` if the tree has no `.config`;
2. builds the kernel `Image` and compiles **both** device trees from the `.dts`
   source above;
3. takes the ramdisk from `rgds/ramdisk.cpio.gz` and the boot header fields from
   `rgds/boot-header.conf` (both from the GammaOS boot image for this board; the
   kernel and device tree are never taken from an image);
4. rebuilds the Rockchip RSCE resource (u-boot reads the runtime device tree from
   it as `rk-kernel.dtb`, next to the boot logo and charge animation) from the
   freshly compiled DTB and the bitmaps in `rgds/rsce/`, and repacks:

```
out/boot_noc.img   CPU 1992 / GPU 800        (stock DTS)
out/boot_oc.img    CPU 2160 / GPU 900 + UV   (overclock DTS)
```

The boot header id is recomputed the way Rockchip u-boot verifies it, so the
images boot on an AVB-enforcing u-boot as well.

To reuse the ramdisk and header of some other boot image instead of the tracked
ones, pass it as the first argument:

```
rgds/build-boot-images.sh /path/to/boot.img
```

## WiFi module

The RTL8821CS SDIO WiFi is an out-of-tree module. Its source is tracked in
`external/rtl8821cs` (Realtek v5.15.9.3, patched to build as a cfg80211 driver
named `8821cs`, with the suspend/resume fix). Build it from the same checkout,
right after the kernel:

```
rgds/build-wifi.sh        # -> out/8821cs.ko
```

It must come from the same tree and compiler as the flashed kernel or it is
rejected at load with `disagrees about version of symbol module_layout`; the
script regenerates `Module.symvers` from the current `vmlinux` first. The
module goes to `/vendor/lib/modules/8821cs.ko` in the GammaOS vendor image.

## Flash

The RG DS images are unsigned (AVB disabled), so no re-signing is needed:

```
fastboot flash boot out/boot_oc.img        # or boot_noc.img
```

The overclock is a device-tree change, but the 2160 MHz point also needs a
bootloader whose ATF carries that PLL rate. Anbernic's shipped u-boot has it; a
from-source u-boot needs `oc/atf-2160-patch.py` from the rg_ds_uboot tree.
`rgds/build-boot-images.sh` prints how to check an image.

## Layout of rgds/

| path | what |
|------|------|
| `board.conf` | board name, compiler, DTB list for this branch |
| `toolchain.sh` | compiler selection shared by the two build scripts |
| `build-boot-images.sh` | kernel + DTBs + RSCE + boot image(s) |
| `build-wifi.sh` | `8821cs.ko` against this tree |
| `ramdisk.cpio.gz`, `boot-header.conf` | GammaOS ramdisk and boot header fields |
| `rsce/` | boot logo and charge animation bitmaps (see `rsce/README.md`) |
| `tools/` | `mkbootimg.py` / `unpack_bootimg.py` (AOSP, Apache-2.0), Rockchip `resource_tool` (rkbin), `dtbcmp.py` |

## Board device trees: labels restored, NOT re-parented (2026-09-16)

`rk3566-anbernic-rg-ds.dts` and `-oc.dts` are decompiles of the shipped DTB. They
now carry their labels and symbolic references back - 821 labels restored, so a
clock reads `<&cru 0x63>` instead of `<0x24 0x63>` - which makes them reviewable.
The tree itself is untouched: same properties, same values, same node order.

### Why they still do not #include the shared base

Re-parenting them onto `rk3566.dtsi` + `rk3568-android.dtsi` was tried and
REVERTED. It produced a tree with identical properties and values, and the device
would not boot: no boot logo, u-boot hands off, the kernel starts, brings up the
secondary CPUs and dies silently. Twice, with two different kernels, which is how
the kernel was ruled out.

The cause is node ORDER. Including a base necessarily emits the base's nodes in
the base's order before the board's own, and the resulting tree had 1065 of 1074
nodes in different positions - starting with `hpll_pinning`, which pins the PLL
the display clocks come from. Node order is functional: the kernel probes
platform devices in tree order and u-boot walks nodes in order.

So "inherit from the shared base" and "produce the same tree this device boots"
are mutually exclusive here. Re-parenting is only viable with a device that can be
tested and re-tested freely, and with the order dependency understood first -
find which nodes actually need their position, rather than assuming none do.

### Verifying a device tree change

`rgds/tools/dtbcmp.py` compares two DTBs semantically: every node addressed by
path, phandle references resolved to their target node (honouring `#clock-cells`
and friends, so clock indices are not mistaken for references), compared as sets
AND in order.

    python3 rgds/tools/dtbcmp.py old.dtb new.dtb
    # -> SEMANTICALLY IDENTICAL (same properties AND same node order)

The order check exists because its absence is what let a reordered tree be flashed
twice. An earlier version of this tool sorted node paths to tolerate phandle
renumbering, reported "identical" for the reordered tree, and was believed.

### Known rough edge

Clock and pin constants are still numeric (`<&cru 0x63>` rather than
`<&cru ACLK_VOP>`). Mapping those to their dt-bindings macros is a further pass.
