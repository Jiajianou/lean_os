#!/usr/bin/env bash
set -euo pipefail

EDK2_TAG="edk2-stable202508"
EDK2_SRC="/tmp/lean-os-ovmf-edk2"
OUT_DIR="build/ovmf"

for tool in git python3 perl clang lld-link iasl; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: '$tool' not found." >&2
        echo "Install with: brew install lld acpica  (llvm-lib/llvm-rc below also need 'brew install llvm')" >&2
        exit 1
    fi
done

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || true)"
if [ -z "$LLVM_PREFIX" ] || [ ! -x "$LLVM_PREFIX/bin/llvm-lib" ]; then
    echo "error: Homebrew's full llvm package not found (need llvm-lib/llvm-rc)." >&2
    echo "Install with: brew install llvm" >&2
    exit 1
fi

if [ -d "$EDK2_SRC" ] && ! git -C "$EDK2_SRC" rev-parse --git-dir >/dev/null 2>&1; then
    echo "edk2 checkout at $EDK2_SRC is not a usable git repository (a" >&2
    echo "partially collected /tmp, most likely) - removing it and cloning again." >&2
    rm -rf "$EDK2_SRC"
fi

if [ ! -d "$EDK2_SRC" ]; then
    echo "Cloning edk2 ($EDK2_TAG)..."
    git clone --branch "$EDK2_TAG" --depth 1 https://github.com/tianocore/edk2.git "$EDK2_SRC"
fi

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

if [ ! -d "$EDK2_SRC/CryptoPkg/Library/OpensslLib/OpensslGen/include" ]; then
    echo "Generating OpenSSL sources for CryptoPkg (one-time, several minutes)..."
    (cd "$EDK2_SRC/CryptoPkg/Library/OpensslLib" && python3 configure.py)
fi

echo "Building BaseTools (host C tools)..."
(cd "$EDK2_SRC" && PYTHON_COMMAND=python3 make -C BaseTools >/dev/null)

SHIM_DIR="/tmp/lean-os-ovmf-shim"
mkdir -p "$SHIM_DIR"
ln -sf "$(command -v clang)" "$SHIM_DIR/clang"
ln -sf "$(command -v lld-link)" "$SHIM_DIR/lld-link"
ln -sf "$LLVM_PREFIX/bin/llvm-lib" "$SHIM_DIR/llvm-lib"

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
    set +u
    source edksetup.sh BaseTools >/dev/null
    set -u
    export CLANG_BIN="$SHIM_DIR/"
    export UNIX_IASL_BIN="$(command -v iasl)"
    build -a X64 -t CLANGPDB -p OvmfPkg/OvmfPkgX64.dsc -b RELEASE -n "$(sysctl -n hw.ncpu)" \
        -D BUILD_SHELL=FALSE
)

mkdir -p "$OUT_DIR"
FV_DIR="$EDK2_SRC/Build/OvmfX64/RELEASE_CLANGPDB/FV"
cp "$FV_DIR/OVMF_CODE.fd" "$OUT_DIR/OVMF_CODE.fd"
cp "$FV_DIR/OVMF_VARS.fd" "$OUT_DIR/OVMF_VARS.fd"

echo "OVMF firmware ready: $OUT_DIR/OVMF_CODE.fd, $OUT_DIR/OVMF_VARS.fd"
