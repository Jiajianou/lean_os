#!/usr/bin/env bash
# Builds OVMF (the open-source UEFI firmware tianocore/edk2 ships) from
# source, for tools/run-qemu.sh and tools/qemu-serial-test.sh to boot
# against (auto-invoked by both if build/ovmf/ isn't there yet). Dev-time
# only, like every other tool this project's toolchain pulls in
# (docs/toolchain.md) - OVMF stands in for real hardware's own firmware,
# never anything the OS image itself ships or links against.
#
# Why build it instead of `brew install`: there is no OVMF formula in
# homebrew-core, and the one third-party tap this machine had lying
# around builds a 2021 firmware (edk2-stable202102) that reliably crashes
# under current QEMU (a VirtioPciDeviceDxe bug unrelated to anything this
# project's own boot code does - reproduced and diagnosed during M24
# bring-up). Building a current release from tianocore's own source
# avoids that entirely.
#
# Why CLANGPDB instead of the GCC5 toolchain most OVMF build guides
# assume: GCC5 wants a Linux-hosted x86_64-elf cross-gcc, which isn't
# what this project's own toolchain uses (docs/toolchain.md's is a bare
# `x86_64-elf` target, not a Linux triple with glibc/crt assumptions edk2's
# GCC5 build rules lean on). CLANGPDB instead cross-compiles straight to
# PE/COFF with clang + lld's `lld-link`, which is exactly the same
# pairing kernel/boot/uefi/boot.c itself is compiled with (see the
# top-level Makefile) - one less toolchain family in the project, not one
# more.
set -euo pipefail

EDK2_TAG="edk2-stable202508"
# The edk2 checkout has to live at a *short* absolute path - not a
# preference, a hard requirement. This LLVM's llvm-rc silently misparses
# its own argv once the input .rc file's absolute path gets long enough
# (reproduced directly: the exact same file, byte-for-byte, compiles fine
# from a short path and fails with a nonsensical "Exactly one input file
# should be provided" from a long one - a handful of edk2's own HII-form
# modules, LogoDxe among them, hit this). A path under this project
# itself (/Users/you/.../lean_os/ovmf-build/edk2/Build/OvmfX64/...) is
# routinely long enough to cross that line depending on where the repo
# happens to be checked out, so this intentionally does *not* live inside
# the project tree the way everything else in this script would suggest.
# /tmp is cleared on reboot; if it's gone, just re-run this script.
EDK2_SRC="/tmp/lean-os-ovmf-edk2"
OUT_DIR="build/ovmf"

for tool in git python3 perl clang lld-link iasl; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: '$tool' not found." >&2
        echo "Install with: brew install lld acpica  (llvm-lib/llvm-rc below also need 'brew install llvm')" >&2
        exit 1
    fi
done

# CLANGPDB needs llvm-lib and llvm-rc, which only the full Homebrew LLVM
# keg ships (Apple's system clang - and lld's own bin/ - don't include
# them). This is the one piece of this script's toolchain that's bigger
# than what building the OS itself needs (kernel/boot/uefi/boot.c compiles
# fine with plain system clang + lld-link alone).
LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || true)"
if [ -z "$LLVM_PREFIX" ] || [ ! -x "$LLVM_PREFIX/bin/llvm-lib" ]; then
    echo "error: Homebrew's full llvm package not found (need llvm-lib/llvm-rc)." >&2
    echo "Install with: brew install llvm" >&2
    exit 1
fi

if [ ! -d "$EDK2_SRC" ]; then
    echo "Cloning edk2 ($EDK2_TAG)..."
    git clone --branch "$EDK2_TAG" --depth 1 https://github.com/tianocore/edk2.git "$EDK2_SRC"
fi

# The submodules OvmfPkgX64's .dec dependency chain reaches (found by
# building and initializing each one build.py complained about missing,
# in order, until it stopped complaining - the last few showed up as one
# batch and weren't individually re-verified as strictly required, so
# this list errs slightly generous rather than risk missing one).
SUBMODULES=(
    BaseTools/Source/C/BrotliCompress/brotli
    MdeModulePkg/Library/BrotliCustomDecompressLib/brotli
    MdePkg/Library/MipiSysTLib/mipisyst
    CryptoPkg/Library/OpensslLib/openssl
    CryptoPkg/Library/MbedTlsLib/mbedtls
    SecurityPkg/DeviceSecurity/SpdmLib/libspdm
    MdeModulePkg/Universal/RegularExpressionDxe/oniguruma
    RedfishPkg/Library/JsonLib/jansson
    UnitTestFrameworkPkg/Library/CmockaLib/cmocka
    UnitTestFrameworkPkg/Library/GoogleTestLib/googletest
    UnitTestFrameworkPkg/Library/SubhookLib/subhook
    MdePkg/Library/BaseFdtLib/libfdt
)
echo "Fetching required submodules..."
(cd "$EDK2_SRC" && git submodule update --init --depth 1 "${SUBMODULES[@]}")

# CryptoPkg vendors OpenSSL as a bare source-code submodule, not a set of
# files edk2's own build understands directly - this generates the actual
# .c/.h files OpensslLibCrypto.inf compiles from (by driving OpenSSL's own
# `perl Configure` for each target the library supports) into OpensslGen/.
# Slow (several `Configure`+`distclean` passes) and its output is fully
# determined by the pinned openssl submodule commit, so - like the git
# clone above - only run it once.
if [ ! -d "$EDK2_SRC/CryptoPkg/Library/OpensslLib/OpensslGen/include" ]; then
    echo "Generating OpenSSL sources for CryptoPkg (one-time, several minutes)..."
    (cd "$EDK2_SRC/CryptoPkg/Library/OpensslLib" && python3 configure.py)
fi

echo "Building BaseTools (host C tools)..."
(cd "$EDK2_SRC" && PYTHON_COMMAND=python3 make -C BaseTools >/dev/null)

# edk2's own tools_def.template CLANGPDB entries look up clang/lld-link/
# llvm-lib/llvm-rc all under one ENV(CLANG_BIN) prefix - this repo's
# actual binaries are split across the system clang, Homebrew's lld, and
# Homebrew's llvm kegs, so this shim directory is just symlinks pointing
# each expected name at wherever it really lives.
SHIM_DIR="/tmp/lean-os-ovmf-shim"
mkdir -p "$SHIM_DIR"
ln -sf "$(command -v clang)" "$SHIM_DIR/clang"
ln -sf "$(command -v lld-link)" "$SHIM_DIR/lld-link"
ln -sf "$LLVM_PREFIX/bin/llvm-lib" "$SHIM_DIR/llvm-lib"

# Not a symlink like the others: edk2's Makefiles invoke rc as
# `llvm-rc /Fo<path> file.rc` (output path glued onto the flag, classic
# rc.exe style) but this LLVM version's llvm-rc only accepts `/Fo <path>`
# (a separate argument) - glued, it miscounts argv and fails every single
# HII-resource module with "Exactly one input file should be provided"
# (confirmed by reproducing the exact same failure standalone, then
# finding the space-separated form works). Splitting `/Fo<path>` into two
# argv entries before forwarding is a straight compatibility shim, not a
# real fix to anything of this project's own.
cat > "$SHIM_DIR/llvm-rc" <<EOF
#!/usr/bin/env bash
args=()
for a in "\$@"; do
    if [[ "\$a" == /Fo?* ]]; then
        args+=("/Fo" "\${a#/Fo}")
    else
        args+=("\$a")
    fi
done
exec "$LLVM_PREFIX/bin/llvm-rc" "\${args[@]}"
EOF
chmod +x "$SHIM_DIR/llvm-rc"

echo "Building OvmfPkgX64 (RELEASE, CLANGPDB)..."
(
    cd "$EDK2_SRC"
    export PYTHON_COMMAND=python3
    # edksetup.sh isn't written to tolerate `set -u` (it references a few
    # variables, like WORKSPACE, before it's the one that defines them) -
    # relax just for this one sourced script, not the rest of this file.
    set +u
    # shellcheck source=/dev/null
    source edksetup.sh BaseTools >/dev/null
    set -u
    export CLANG_BIN="$SHIM_DIR/"
    export UNIX_IASL_BIN="$(command -v iasl)"
    # BUILD_SHELL=FALSE: the built-in UEFI Shell (and its LinuxInitrd
    # helper command) aren't needed - this project's own BOOTX64.EFI is
    # the only thing that ever needs to run - so there's no reason to pay
    # for building it.
    build -a X64 -t CLANGPDB -p OvmfPkg/OvmfPkgX64.dsc -b RELEASE -n "$(sysctl -n hw.ncpu)" \
        -D BUILD_SHELL=FALSE
)

mkdir -p "$OUT_DIR"
FV_DIR="$EDK2_SRC/Build/OvmfX64/RELEASE_CLANGPDB/FV"
cp "$FV_DIR/OVMF_CODE.fd" "$OUT_DIR/OVMF_CODE.fd"
cp "$FV_DIR/OVMF_VARS.fd" "$OUT_DIR/OVMF_VARS.fd"

echo "OVMF firmware ready: $OUT_DIR/OVMF_CODE.fd, $OUT_DIR/OVMF_VARS.fd"
