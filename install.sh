#!/usr/bin/env bash
# ScreenMate installer — Linux / macOS
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/KOFA14888/gpt/arena/d7d4a28f-gpt/install.sh | bash
set -euo pipefail

REPO="https://github.com/KOFA14888/gpt"
BRANCH="${SCREENMATE_BRANCH:-arena/d7d4a28f-gpt}"  # default branch has no sources yet
ASSET_LINUX="$REPO/releases/latest/download/screenmate-linux-x64.tar.gz"
BIN_DIR="${SCREENMATE_BIN_DIR:-$HOME/.local/bin}"
BIN="$BIN_DIR/screenmate"

echo "ScreenMate installer (Linux/macOS)"

mkdir -p "$BIN_DIR"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

installed=0

# 1) prebuilt x86_64 glibc binary from GitHub Releases (fast path)
arch="$(uname -m)"
os="$(uname -s)"
if [ "$os" = "Linux" ] && { [ "$arch" = "x86_64" ] || [ "$arch" = "amd64" ]; }; then
  if curl -fsSL "$ASSET_LINUX" -o "$TMP/sm.tar.gz" 2>/dev/null ||
     wget -qO "$TMP/sm.tar.gz" "$ASSET_LINUX" 2>/dev/null; then
    tar xzf "$TMP/sm.tar.gz" -C "$TMP"
    if [ -f "$TMP/screenmate" ]; then
      install -m 755 "$TMP/screenmate" "$BIN"
      installed=1
      echo "Installed (prebuilt): $BIN"
    fi
  fi
fi

# 2) fallback: clone + build from source (needs git, g++/clang++, cmake, libcurl)
if [ "$installed" -eq 0 ]; then
  echo "Prebuilt not available for this platform — building from source..."
  command -v git   >/dev/null || { echo "error: git not found"; exit 1; }
  command -v cmake >/dev/null || { echo "error: cmake not found (sudo apt install cmake)"; exit 1; }
  command -v g++   >/dev/null || command -v clang++ >/dev/null ||
    { echo "error: C++ compiler not found (sudo apt install build-essential)"; exit 1; }

  SRC="$TMP/src"
  git clone --depth 1 --branch "$BRANCH" "$REPO" "$SRC"
  cd "$SRC"
  cmake -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build --parallel
  install -m 755 build/screenmate "$BIN"
  echo "Installed (built from source): $BIN"
fi

# 3) PATH hint
case ":$PATH:" in
  *":$BIN_DIR:"*) ;;
  *)
    echo
    echo "Add to PATH (e.g. in ~/.bashrc / ~/.zshrc):"
    echo "  export PATH=\"$BIN_DIR:\$PATH\""
    ;;
esac

echo
echo "Done. Run:"
echo "  screenmate            # chat UI"
echo "  screenmate auth login # set API key"
