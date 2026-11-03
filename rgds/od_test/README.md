# Motion test tools for the RG DS Plus panels

- `odtest.c`: takes DRM master on one panel (stop the nano home first) and shows a four-box
  motion pattern (mode 0/1, with an optional in-tool overdrive) or a ten-patch response
  calibration pattern (mode 2). Knobs are reread from a file while it runs; see the header.
  Build: `aarch64-linux-gnu-gcc -O2 -static -o odtest odtest.c`
- `bfi_test.c`: one-off module that switches one backlight PWM to a one-frame period, to see
  whether the backlight boost follows a 60 Hz pulse. Build out of tree against this kernel:
  `make -C <kernel> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=$PWD modules`
