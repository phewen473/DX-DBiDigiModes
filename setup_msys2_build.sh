#!/usr/bin/env bash
# =============================================================================
#  setup_msys2_build.sh  -  one-time build environment setup for WSJT-X 3.0.2
#
#  Run from the "MSYS2 MINGW64" shell (NOT the plain "MSYS2 MSYS" shell):
#
#      cd /c/path/to/DX-DBiDigiModes
#      ./setup_msys2_build.sh
#
#  What it does (each step is skipped if already done):
#    1. Installs the MSYS2 / MINGW64 packages WSJT-X needs
#    2. Installs asciidoctor (Ruby gem) for the user guide
#    3. Makes "dumpcpp" available (MSYS2 names it dumpcpp-qt5.exe)
#    4. Builds libusb-1.0 (no longer packaged for MINGW64) and Hamlib
#       into a private prefix
#    5. Finds OmniRig (offers to download/install it if missing)
#    6. Runs the first CMake configure into an out-of-tree build folder
#
#  Settings mirror the project's own .github/workflows/build-windows.yml.
#
#  Optional environment overrides, e.g.  SKIP_DOCS=1 ./setup_msys2_build.sh
#    WORK_DIR      where hamlib-src, hamlib-prefix and the build go
#                  (default: the folder that contains this source tree)
#    BUILD_DIR     CMake build folder   (default: $WORK_DIR/<source folder name>-build)
#    HAMLIB_BRANCH Hamlib git tag/branch (default: 4.7.0, same as upstream CI)
#    SKIP_DOCS=1   don't build the user guide (skips asciidoctor)
#    SKIP_MAP65=1  don't build MAP65 (use if gfortran rejects its old Fortran)
#    SKIP_TESTS=1  don't build the test programs
#    REBUILD_HAMLIB=1  rebuild libusb and Hamlib even if already installed
#    LIBUSB_TAG    libusb git tag (default: v1.0.30)
#    OMNIRIG_EXE   full Windows or MSYS path to OmniRig.exe if not auto-found
# =============================================================================
set -euo pipefail

# ---------- helpers ----------------------------------------------------------
c_info=$'\e[1;36m'; c_ok=$'\e[1;32m'; c_warn=$'\e[1;33m'; c_err=$'\e[1;31m'; c_off=$'\e[0m'
step() { echo; echo "${c_info}==> $*${c_off}"; }
ok()   { echo "${c_ok}    $*${c_off}"; }
warn() { echo "${c_warn}    WARNING: $*${c_off}"; }
die()  { echo "${c_err}ERROR: $*${c_off}" >&2; exit 1; }

# ---------- sanity checks ----------------------------------------------------
[[ "${MSYSTEM:-}" == "MINGW64" ]] || die "Run this from the 'MSYS2 MINGW64' shell (MSYSTEM is '${MSYSTEM:-unset}')."

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[[ -f "$SRC_DIR/CMakeLists.txt" && -f "$SRC_DIR/widgets/mainwindow.cpp" ]] \
  || die "This script must live in the root of the WSJT-X source tree."

WORK_DIR="${WORK_DIR:-$(dirname "$SRC_DIR")}"
BUILD_DIR="${BUILD_DIR:-$WORK_DIR/$(basename "$SRC_DIR")-build}"
HAMLIB_BRANCH="${HAMLIB_BRANCH:-4.7.0}"
LIBUSB_TAG="${LIBUSB_TAG:-v1.0.30}"
LIBUSB_SRC="$WORK_DIR/libusb-src"
HAMLIB_SRC="$WORK_DIR/hamlib-src"
HAMLIB_PREFIX="$WORK_DIR/hamlib-prefix"
JOBS="$(nproc)"

echo "Source tree : $SRC_DIR"
echo "Build folder: $BUILD_DIR"
echo "Hamlib      : $HAMLIB_BRANCH -> $HAMLIB_PREFIX"

# ---------- 1. packages ------------------------------------------------------
step "1/6  Installing MSYS2 packages"
echo "    (Tip: run 'pacman -Syu' first and restart the shell if it asks you to;"
echo "     MSYS2 discourages installing onto an out-of-date system.)"
PKGS=(
  git make tar unzip autoconf automake libtool
  mingw-w64-x86_64-gcc
  mingw-w64-x86_64-gcc-fortran
  mingw-w64-x86_64-cmake
  mingw-w64-x86_64-pkgconf
  mingw-w64-x86_64-qt5-base
  mingw-w64-x86_64-qt5-multimedia
  mingw-w64-x86_64-qt5-serialport
  mingw-w64-x86_64-qt5-svg
  mingw-w64-x86_64-qt5-tools
  mingw-w64-x86_64-qt5-websockets
  mingw-w64-x86_64-qt5-activeqt
  mingw-w64-x86_64-fftw
  mingw-w64-x86_64-libmariadbclient   # DX-DBi v2R0: MariaDB client for Qt's qsqlmysql (WSPR database)
  mingw-w64-x86_64-boost
  mingw-w64-x86_64-portaudio
  mingw-w64-x86_64-nsis
  mingw-w64-x86_64-ruby
)
pacman -S --needed --noconfirm "${PKGS[@]}"
ok "Packages installed."
echo "    gcc      : $(gcc --version | head -1)"
echo "    gfortran : $(gfortran --version | head -1)"
echo "    cmake    : $(cmake --version | head -1)"

# ---------- 2. asciidoctor ---------------------------------------------------
step "2/6  asciidoctor (user guide)"
if [[ "${SKIP_DOCS:-0}" == "1" ]]; then
  ok "Skipped (SKIP_DOCS=1)."
elif command -v asciidoctor >/dev/null 2>&1; then
  ok "Already installed: $(asciidoctor --version | head -1)"
else
  gem install asciidoctor --no-document
  hash -r
  command -v asciidoctor >/dev/null 2>&1 \
    || die "asciidoctor installed but not on PATH. Add Ruby's gem bin folder to PATH, or rerun with SKIP_DOCS=1."
  ok "Installed: $(asciidoctor --version | head -1)"
fi

# ---------- 3. dumpcpp -------------------------------------------------------
step "3/6  dumpcpp (needed to generate OmniRig.h)"
if command -v dumpcpp >/dev/null 2>&1; then
  ok "Found: $(command -v dumpcpp)"
elif [[ -x /mingw64/bin/dumpcpp-qt5.exe ]]; then
  cp -f /mingw64/bin/dumpcpp-qt5.exe /mingw64/bin/dumpcpp.exe
  ok "Created /mingw64/bin/dumpcpp.exe from dumpcpp-qt5.exe"
else
  die "dumpcpp-qt5.exe not found - is mingw-w64-x86_64-qt5-activeqt installed?"
fi

# ---------- 4. libusb + Hamlib ----------------------------------------------
# MSYS2 dropped mingw-w64-x86_64-libusb (libusb-1.0) from the MINGW64 repo, so
# build it from source into the same private prefix that Hamlib uses.
step "4/6  libusb $LIBUSB_TAG and Hamlib $HAMLIB_BRANCH"
export PKG_CONFIG_PATH="$HAMLIB_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
if [[ -f "$HAMLIB_PREFIX/include/libusb-1.0/libusb.h" && "${REBUILD_HAMLIB:-0}" != "1" ]]; then
  ok "libusb already installed in $HAMLIB_PREFIX"
else
  [[ -d "$LIBUSB_SRC/.git" ]] || git clone --depth 1 --branch "$LIBUSB_TAG" https://github.com/libusb/libusb.git "$LIBUSB_SRC"
  pushd "$LIBUSB_SRC" >/dev/null
  [[ -f Makefile ]] && make distclean >/dev/null 2>&1 || true
  ./bootstrap.sh
  ./configure --prefix="$HAMLIB_PREFIX" --enable-shared --disable-static
  make -j"$JOBS"
  make install
  popd >/dev/null
  [[ -f "$HAMLIB_PREFIX/include/libusb-1.0/libusb.h" ]] || die "libusb install failed."
  ok "libusb installed in $HAMLIB_PREFIX"
fi
if [[ -f "$HAMLIB_PREFIX/include/hamlib/rig.h" && "${REBUILD_HAMLIB:-0}" != "1" ]]; then
  ok "Already installed in $HAMLIB_PREFIX (set REBUILD_HAMLIB=1 to rebuild)."
else
  if [[ ! -d "$HAMLIB_SRC/.git" ]]; then
    git clone --depth 1 --branch "$HAMLIB_BRANCH" https://github.com/Hamlib/Hamlib.git "$HAMLIB_SRC"
  else
    ok "Reusing existing clone at $HAMLIB_SRC"
  fi
  pushd "$HAMLIB_SRC" >/dev/null
  # Hamlib 4.7.0 calls assert() in lib/precise_time.c without <assert.h>;
  # GCC 14+ rejects implicit declarations, so add the missing include.
  if grep -q 'assert(' lib/precise_time.c 2>/dev/null && ! grep -q '<assert.h>' lib/precise_time.c; then
    sed -i '1i #include <assert.h>' lib/precise_time.c
    ok "Patched lib/precise_time.c (added #include <assert.h>)"
  fi
  [[ -f Makefile ]] && make distclean >/dev/null 2>&1 || true
  ./bootstrap
  ./configure \
    --prefix="$HAMLIB_PREFIX" \
    --enable-shared --disable-static \
    --without-cxx-binding \
    --without-readline \
    CFLAGS="-DNDEBUG -g -O2 -fdata-sections -ffunction-sections" \
    LDFLAGS="-Wl,--gc-sections"
  make -j"$JOBS"
  make install-strip
  popd >/dev/null
  [[ -f "$HAMLIB_PREFIX/include/hamlib/rig.h" ]] || die "Hamlib install failed."
  ok "Hamlib installed in $HAMLIB_PREFIX"
fi

# ---------- 5. OmniRig -------------------------------------------------------
step "5/6  OmniRig (COM type library for the OmniRig rig-control option)"
find_omnirig() {
  local p
  for p in "/c/Program Files (x86)/Afreet/OmniRig/OmniRig.exe" \
           "/c/Program Files/Afreet/OmniRig/OmniRig.exe"; do
    [[ -f "$p" ]] && { echo "$p"; return 0; }
  done
  return 1
}
OMNIRIG=""
if [[ -n "${OMNIRIG_EXE:-}" ]]; then
  OMNIRIG="$(cygpath -u "$OMNIRIG_EXE")"
  [[ -f "$OMNIRIG" ]] || die "OMNIRIG_EXE does not exist: $OMNIRIG_EXE"
elif OMNIRIG="$(find_omnirig)"; then
  :
else
  warn "OmniRig is not installed. WSJT-X's Windows build requires its type library."
  read -r -p "    Download and install OmniRig from dxatlas.com now? [y/N] " ans
  if [[ "$ans" =~ ^[Yy] ]]; then
    tmp="$(mktemp -d)"
    curl -fL -o "$tmp/OmniRig.zip" "https://www.dxatlas.com/OmniRig/Files/OmniRig.zip"
    (cd "$tmp" && unzip -o -q OmniRig.zip)
    setup_exe="$(find "$tmp" -iname '*.exe' | head -1)"
    [[ -n "$setup_exe" ]] || die "No installer found inside OmniRig.zip"
    echo "    Launching installer (Windows will ask for administrator approval)..."
    cmd //c "$(cygpath -w "$setup_exe")" /SILENT /NORESTART
    OMNIRIG="$(find_omnirig)" || die "OmniRig install did not complete; install it manually and rerun."
  else
    die "Install OmniRig (https://www.dxatlas.com/OmniRig/) or set OMNIRIG_EXE, then rerun."
  fi
fi
ok "Using $OMNIRIG"

# ---------- 6. CMake configure ----------------------------------------------
step "6/6  CMake configure"
onoff() { [[ "$1" == "1" ]] && echo ON || echo OFF; }
GEN_DOCS=$([[ "${SKIP_DOCS:-0}" == "1" ]] && echo OFF || echo ON)
BUILD_TESTS=$([[ "${SKIP_TESTS:-0}" == "1" ]] && echo OFF || echo ON)
SKIP_MAP65_OPT=$(onoff "${SKIP_MAP65:-0}")

cmake -G "MSYS Makefiles" -S "$SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HAMLIB_PREFIX" \
  -DOMNIRIG_TYPE_LIB="$OMNIRIG" \
  -DCMAKE_Fortran_FLAGS="-fallow-argument-mismatch -std=legacy" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DWSJT_SKIP_MANPAGES=ON \
  -DWSJT_GENERATE_DOCS="$GEN_DOCS" \
  -DWSJT_BUILD_TESTS="$BUILD_TESTS" \
  -DWSJT_SKIP_MAP65="$SKIP_MAP65_OPT" \
  -Wno-dev

echo
ok "Configure finished successfully."
cat <<NEXT

Next steps (from this MINGW64 shell):

  Build:    cmake --build "$BUILD_DIR" -j$JOBS
  Test:     (cd "$BUILD_DIR" && ctest --output-on-failure)
  Install:  cmake --install "$BUILD_DIR" --prefix "$WORK_DIR/wsjtx-install"

To run wsjtx.exe straight from the build folder, the Hamlib DLL folder
must be on PATH:   export PATH="$HAMLIB_PREFIX/bin:\$PATH"
NEXT
