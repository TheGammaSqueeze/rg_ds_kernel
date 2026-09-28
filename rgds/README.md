# RG DS Plus kernel build

This is the Linux 6.1 kernel for the Anbernic RG DS Plus (Rockchip RK3568, dual
DSI panels, microSD boot). This branch (`rgdsplus-main`) builds the RG DS Plus;
the `main` branch of the same repository builds the RG DS with the same scripts
and driver code, the two differ in their board device trees, boot logos and
`rgds/board.conf`.

Everything a boot image needs is in this tree: kernel, board device tree as
source, the RSCE boot logo and charge animation bitmaps, the GammaOS ramdisk and
boot header parameters, and the WiFi driver source. The only thing to install
is the compiler.

The board device tree is kept **as source**, not as a prebuilt blob:

| file | result |
|------|--------|
| `arch/arm64/boot/dts/rockchip/rk3568-anbernic-rg-ds-plus.dts` | CPU per-bin points up to 2160 MHz, GPU 800 MHz, AW88166 speaker amp, microSD UHS-I |

There is one device tree and one boot image for the Plus; the per-bin CPU
voltages live in it and the kernel picks the bin from the OTP.

## Toolchain

The RG DS Plus kernel is built with the GNU cross compiler
(`rgds/board.conf`, `TOOLCHAIN=gcc`):

```
sudo apt install gcc-aarch64-linux-gnu python3 make flex bison bc libssl-dev libelf-dev
```

The prefix defaults to `aarch64-linux-gnu-`; set `CROSS_COMPILE` for another.
Every module loaded into the kernel must come from the same compiler and tree
(see WiFi module below); that is why the compiler is pinned per board.

## Build a kernel + flashable boot image

```
rgds/build-boot-images.sh
```

That is the whole build on a fresh clone. The script:

1. applies `rockchip_rgds_defconfig` if the tree has no `.config`;
2. builds the kernel `Image` and compiles the device tree from the `.dts` source
   above;
3. takes the ramdisk from `rgds/ramdisk.cpio.gz` and the boot header fields from
   `rgds/boot-header.conf` (both from the GammaOS boot image for this board, its
   `androidboot.boot_devices=fe2b0000.mmc,fe2c0000.mmc` cmdline included; the
   kernel and device tree are never taken from an image);
4. rebuilds the Rockchip RSCE resource (u-boot reads the runtime device tree from
   it as `rk-kernel.dtb`, next to the boot logo and charge animation) from the
   freshly compiled DTB and the bitmaps in `rgds/rsce/`, and repacks:

```
out/boot.img
```

The boot header id is recomputed the way Rockchip u-boot verifies it, so the
image boots on an AVB-enforcing u-boot as well.

To reuse the ramdisk and header of some other boot image instead of the tracked
ones, pass it as the first argument:

```
rgds/build-boot-images.sh /path/to/boot.img
```

The recovery image is not built here; GammaOS builds it from the same kernel and
device tree with its own recovery ramdisk.

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
script regenerates `Module.symvers` from the current `vmlinux` first. On the
Plus the module ships in `vendor_dlkm` (`/vendor/lib/modules/8821cs.ko`).

## Flash

The Plus boots from microSD; from fastbootd on the device:

```
fastboot flash boot out/boot.img
```

The 2160 MHz CPU point needs a bootloader whose ATF carries that PLL rate; the
GammaOS u-boot for the Plus does.

## Layout of rgds/

| path | what |
|------|------|
| `board.conf` | board name, compiler, DTB list for this branch |
| `toolchain.sh` | compiler selection shared by the two build scripts |
| `build-boot-images.sh` | kernel + DTB + RSCE + boot image |
| `build-wifi.sh` | `8821cs.ko` against this tree |
| `ramdisk.cpio.gz`, `boot-header.conf` | GammaOS ramdisk and boot header fields |
| `rsce/` | boot logo and charge animation bitmaps (see `rsce/README.md`) |
| `tools/` | `mkbootimg.py` / `unpack_bootimg.py` (AOSP, Apache-2.0), Rockchip `resource_tool` (rkbin), `dtbcmp.py` |

## Verifying a device tree change

`rgds/tools/dtbcmp.py` compares two DTBs semantically: every node addressed by
path, phandle references resolved to their target node (honouring `#clock-cells`
and friends), compared as sets AND in order, because node order is functional on
these boards (the kernel probes platform devices in tree order and u-boot walks
nodes in order).

    python3 rgds/tools/dtbcmp.py old.dtb new.dtb
    # -> SEMANTICALLY IDENTICAL (same properties AND same node order)
