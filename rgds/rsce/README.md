# Boot RSCE bitmaps

The boot and recovery images carry a Rockchip RSCE resource area holding the
runtime device tree (`rk-kernel.dtb`) plus the boot logo and the charge
animation. These are the bitmaps for that area. They used to be lifted out of
a prebuilt boot image at build time, which meant the logos were not in the tree
and the boot image could not be rebuilt from source alone. The build scripts
now take them from here.

To change the boot logo, replace `logo.bmp` and `logo_kernel.bmp` and rebuild.
The Paint.NET sources they were exported from live alongside them as
`logo.pdn` and `logo_kernel.pdn`, so the artwork stays editable. Only the BMPs
are read by the build; the `.pdn` files are kept for future edits.

## Format, which is not negotiable

u-boot's BMP reader and `resource_tool` only accept these exact encodings:

| File                     | Size    | Encoding                                |
|--------------------------|---------|-----------------------------------------|
| `logo.bmp`               | 1024x768 | 8- or 24-bit, uncompressed (BI_RGB)    |
| `logo_kernel.bmp`        | 1024x768 | 8- or 24-bit, uncompressed (BI_RGB)    |
| `battery_0..5.bmp`       | 220x110 | 8-bit palettised, RLE8 compressed       |
| `battery_fail.bmp`       | 220x110 | 8-bit palettised, RLE8 compressed       |

8-bit palettised logos are fine, device verified on the Plus, and are about a
third of the size of the 24-bit export. What does not work is 32-bit or
BI_BITFIELDS, which most editors will hand you by default; those render as
garbage or not at all. Check with `file x.bmp` before committing: it must say
`8` or `24`, never `32`, and the battery frames must say `1 compression`.

The logos are now the panel's native 1024x768, so u-boot does not scale them.
Validate any change on a real cold boot, not a warm reboot, because the
bootloader logo path only runs on a cold start.

Entry order in the RSCE matters: `rk-kernel.dtb` must be packed first, then the
bitmaps. The build scripts handle that.
