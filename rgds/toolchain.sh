# toolchain.sh - shared by rgds/build-boot-images.sh and rgds/build-wifi.sh.
#
# Sourced, not executed. Expects $HERE (the rgds/ directory) and $KDIR (the
# kernel tree root) to be set, reads rgds/board.conf, and leaves:
#
#   BOARD, TOOLCHAIN, DTBS        from board.conf
#   MAKE=(...)                    the make invocation for this board's compiler
#   TOOLCHAIN_DESC                one line describing what was picked, for logs
#
# Which compiler a board uses is part of board.conf on purpose: the flashed
# kernel and every out-of-tree module loaded into it (8821cs.ko) must come from
# the same compiler and the same tree, or the module is rejected with
# "disagrees about version of symbol module_layout".
#
#   TOOLCHAIN=clang  Android clang, LLVM=1. The RG DS tree was built and
#                    verified with r487747c. Set CLANGBIN to its bin/ directory;
#                    without CLANGBIN the clang found on PATH is used and a
#                    warning is printed if it is another version.
#   TOOLCHAIN=gcc    aarch64-linux-gnu- cross gcc (Debian/Ubuntu package
#                    gcc-aarch64-linux-gnu). Override the prefix with
#                    CROSS_COMPILE.

# shellcheck disable=SC1091
. "$HERE/board.conf"
: "${BOARD:?board.conf must set BOARD}"
: "${TOOLCHAIN:?board.conf must set TOOLCHAIN}"
: "${DTBS:?board.conf must set DTBS}"

JOBS="${JOBS:-$(nproc)}"
CROSS="${CROSS_COMPILE:-aarch64-linux-gnu-}"
tc_die() { printf 'error: %s\n' "$*" >&2; exit 1; }

case "$TOOLCHAIN" in
  clang)
    if [ -n "${CLANGBIN:-}" ]; then
      [ -x "$CLANGBIN/clang" ] || tc_die "CLANGBIN=$CLANGBIN has no clang"
    else
      CLANGBIN="$(dirname "$(command -v clang 2>/dev/null || true)")"
      [ -n "$CLANGBIN" ] && [ -x "$CLANGBIN/clang" ] || tc_die \
        "no clang found. Set CLANGBIN to an Android clang bin/ directory (see rgds/README.md, Toolchain)"
    fi
    export PATH="$CLANGBIN:$PATH"
    CLANG_VER="$("$CLANGBIN/clang" --version | head -1)"
    case "$CLANG_VER" in
      *"${CLANG_WANT:-r487747c}"*) ;;
      *) printf 'warning: %s is not Android clang %s; the kernel will build, but 8821cs.ko must then be rebuilt from this tree too (rgds/build-wifi.sh does that)\n' \
           "$CLANG_VER" "${CLANG_WANT:-r487747c}" >&2 ;;
    esac
    MAKE=(make -C "$KDIR" ARCH=arm64 LLVM="$CLANGBIN/" LLVM_IAS=1 CROSS_COMPILE="$CROSS" -j"$JOBS")
    TOOLCHAIN_DESC="clang ($CLANG_VER) from $CLANGBIN"
    ;;
  gcc)
    command -v "${CROSS}gcc" >/dev/null 2>&1 || tc_die \
      "${CROSS}gcc not found (install gcc-aarch64-linux-gnu or set CROSS_COMPILE)"
    MAKE=(make -C "$KDIR" ARCH=arm64 CROSS_COMPILE="$CROSS" -j"$JOBS")
    TOOLCHAIN_DESC="gcc ($("${CROSS}gcc" --version | head -1))"
    ;;
  *) tc_die "board.conf: TOOLCHAIN must be clang or gcc, got '$TOOLCHAIN'" ;;
esac
