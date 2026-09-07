# Anbernic RG DS - device trees and overclock

Two board device trees are built from source here (added to the dtb Makefile):

| dts | max CPU | GPU | notes |
|-----|---------|-----|-------|
| `rk3566-anbernic-rg-ds.dts`    | 1992 MHz | 800 MHz | stock |
| `rk3566-anbernic-rg-ds-oc.dts` | 2160 MHz | 900 MHz | overclock + undervolt |

`make dtbs` (or `make rockchip/rk3566-anbernic-rg-ds{,-oc}.dtb`) produces
`rk3566-anbernic-rg-ds.dtb` and `rk3566-anbernic-rg-ds-oc.dtb`. Both are the
authoritative runtime device trees (verified byte-for-content against the
shipped device). They are decompiled-derived (flattened, numeric phandles) -
buildable and inspectable, but not yet refactored to `#include` the SoC dtsi;
that refactor is a follow-up.

## Overclock delta (stock -> oc)

Purely a device-tree change (no ATF/u-boot patch needed - the rkbin BL31 in
u-boot already supports the 2160 PLL rate). Applied in the CPU and GPU OPP
tables of `-oc.dts`:

- **CPU**: add `opp-2160000000` (2160 MHz) as the new top OPP.
- **GPU (Mali-G52)**: top OPP raised 800 -> 900 MHz.
- **Undervolt** (all three fields of every `opp-microvolt` / `opp-microvolt-L0..L3`
  lowered together, or the Rockchip AVS drifts voltage back up):
  - -25 mV across all OPPs,
  - additional -50 mV on the top OC entries (CPU 2160, GPU 900).
  - e.g. CPU top 1050 -> 1000 mV, GPU top 1025 -> 975 mV.

Measured: sha256 of 64 MiB best-of-5 = ~232-245 ms at 2160 vs ~346 ms stock
(real gain, not a PLL-divider artefact); GPU load ~20% -> ~13% at the same
workload. Validated under sustained CPU+GPU load, thermals ~49-50 C.

Flash `boot_noc.img` (stock dtb) or `boot_oc.img` (oc dtb) - see the build
script. u-boot is common to both.
