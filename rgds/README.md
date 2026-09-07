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
