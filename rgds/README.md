# RG DS kernel build

This is the Linux 6.1 kernel for the Anbernic RG DS (Rockchip RK3566, dual
640x480 DSI panels), built as a functional drop-in for the stock 6.1.141 kernel.

The board device tree is kept here **as source**, not as a prebuilt blob:

| file | result |
|------|--------|
| `arch/arm64/boot/dts/rockchip/rk3566-anbernic-rg-ds.dts`    | stock clocks: CPU 1992 MHz, GPU 800 MHz |
| `arch/arm64/boot/dts/rockchip/rk3566-anbernic-rg-ds-oc.dts` | overclock: CPU 2160 MHz, GPU 900 MHz + undervolt |

Both are compiled by the normal kernel `dtbs` build. The overclock delta (which
OPP/voltage lines differ) is documented in
`arch/arm64/boot/dts/rockchip/RG_DS_OVERCLOCK.md`.

## Toolchain

Built with Android clang `r487747c` (`LLVM=1`), the same toolchain as stock.
`gcc` will not build this tree (it uses clang-only kernel cflags such as
`-ftrivial-auto-var-init=zero`). Point `CLANGBIN` at your clang `bin/` directory
if it is not at the default path baked into the scripts.

## Build a kernel + two flashable boot images

```
# 1. configure
make ARCH=arm64 rockchip_rgds_defconfig

# 2. build the kernel and both boot images (host-side, no device / root needed)
rgds/build-boot-images.sh /path/to/stock-boot.img
```

`build-boot-images.sh`:

1. builds the kernel `Image` and compiles **both** device trees from the `.dts`
   source above;
2. takes the ramdisk, boot logo / battery bitmaps and boot-header parameters from
   the stock boot image you pass in (the kernel and device tree are always
   rebuilt from this tree, never taken from that image);
3. rebuilds the Rockchip RSCE resource (the RG DS keeps the runtime device tree
   there as `rk-kernel.dtb`) with the freshly compiled DTB, and repacks two
   images:

```
out/boot_noc.img   CPU 1992 / GPU 800        (stock DTS)
out/boot_oc.img    CPU 2160 / GPU 900 + UV   (overclock DTS)
```

Get the stock boot image once from your device:

```
adb pull /dev/block/by-name/boot stock-boot.img     # root shell or recovery
```

or use the `boot.img` from a GammaOS release for this device.

## Flash

The RG DS images are unsigned (AVB disabled), so no re-signing is needed:

```
fastboot flash boot out/boot_oc.img        # or boot_noc.img
```

The overclock is purely a device-tree change; the shipped u-boot / ATF already
support the 2160 MHz PLL, so the same u-boot runs both images and no separate
u-boot flash is needed to toggle it.

## WiFi module

The RTL8821CS SDIO WiFi is an out-of-tree module, built against this kernel from
<https://github.com/u-osmi/rtl8821cs-arm64>. It must be built with this exact
kernel's `Module.symvers` and the clang toolchain above, or the loaded `.ko` is
rejected with a `module_layout` version mismatch.

## Tools

`rgds/tools/` vendors the small helpers the boot-image build needs:
`mkbootimg.py` / `unpack_bootimg.py` (AOSP, Apache-2.0) and Rockchip's
`resource_tool` (from `rockchip-linux/rkbin`). They are build helpers only; the
device tree itself is always compiled from the `.dts` source in this tree.

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
