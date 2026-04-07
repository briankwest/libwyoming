#!/bin/bash
#
# package-voice.sh — Download a Piper voice and build a .deb
#
# Usage: ./package-voice.sh en_US-lessac-high
#        ./package-voice.sh en_US-amy-medium
#
# Downloads from huggingface.co/rhasspy/piper-voices, creates a deb
# that installs to /usr/share/wyoming/voices/<name>/
#
set -euo pipefail

VOICE="${1:?Usage: $0 <voice-name> (e.g. en_US-lessac-high)}"
VERSION="1.0.0"
MAINTAINER="Brian West <brian@kerchunk.net>"
BASE_URL="https://huggingface.co/rhasspy/piper-voices/resolve/main"

# Parse voice name: en_US-lessac-high → lang=en, country=US, name=lessac, quality=high
IFS='-' read -r lang_country name quality <<< "$VOICE"
LANG_CODE="${lang_country%%_*}"
COUNTRY="${lang_country#*_}"
VOICE_DIR="${LANG_CODE}/${lang_country}/${name}/${quality}"

# Debian package name: wyoming-voice-en-us-lessac-high (lowercase)
PKG_NAME="wyoming-voice-$(echo "$VOICE" | tr 'A-Z_' 'a-z-')"

echo "=== Packaging voice: $VOICE ==="
echo "  Package: $PKG_NAME"
echo "  URL: $BASE_URL/$VOICE_DIR/"

# Create build directory
BUILD_DIR=$(mktemp -d)
trap "rm -rf $BUILD_DIR" EXIT

INSTALL_DIR="$BUILD_DIR/usr/share/wyoming/voices/$VOICE"
mkdir -p "$INSTALL_DIR"

# Download model files
echo "Downloading model.onnx..."
curl -sL "$BASE_URL/$VOICE_DIR/${lang_country}-${name}-${quality}.onnx" \
  -o "$INSTALL_DIR/model.onnx"

echo "Downloading model.onnx.json..."
curl -sL "$BASE_URL/$VOICE_DIR/${lang_country}-${name}-${quality}.onnx.json" \
  -o "$INSTALL_DIR/model.onnx.json"

# Verify downloads
if [ ! -s "$INSTALL_DIR/model.onnx" ]; then
  echo "ERROR: Failed to download model.onnx" >&2
  exit 1
fi

MODEL_SIZE=$(stat -c%s "$INSTALL_DIR/model.onnx")
echo "  Model size: $(( MODEL_SIZE / 1024 / 1024 ))MB"

# Extract sample rate from config
SAMPLE_RATE=$(python3 -c "import json; d=json.load(open('$INSTALL_DIR/model.onnx.json')); print(d.get('audio',{}).get('sample_rate',22050))" 2>/dev/null || echo "22050")
echo "  Sample rate: $SAMPLE_RATE Hz"

# Build debian package
DEBIAN_DIR="$BUILD_DIR/DEBIAN"
mkdir -p "$DEBIAN_DIR"

INSTALLED_SIZE=$(du -sk "$BUILD_DIR/usr" | awk '{print $1}')

cat > "$DEBIAN_DIR/control" << EOF
Package: $PKG_NAME
Version: $VERSION
Architecture: all
Section: sound
Priority: optional
Maintainer: $MAINTAINER
Depends: wyoming-tools
Installed-Size: $INSTALLED_SIZE
Description: Piper voice: $VOICE ($SAMPLE_RATE Hz)
 Piper TTS voice model for the Wyoming protocol.
 .
 Voice: $name ($quality quality)
 Language: ${lang_country}
 Sample rate: $SAMPLE_RATE Hz
 .
 Install path: /usr/share/wyoming/voices/$VOICE/
EOF

# Build .deb
DEB_FILE="${PKG_NAME}_${VERSION}_all.deb"
dpkg-deb --build "$BUILD_DIR" "$DEB_FILE"

echo "=== Built: $DEB_FILE ==="
echo "  Install: sudo dpkg -i $DEB_FILE"
echo "  Use: wyoming-piper-server --model /usr/share/wyoming/voices/$VOICE/model.onnx"
