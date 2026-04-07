#!/bin/bash
#
# package-model.sh — Download a Sherpa-ONNX ASR model and build a .deb
#
# Usage: ./package-model.sh whisper-tiny.en
#        ./package-model.sh whisper-base.en
#
# Downloads from sherpa-onnx GitHub releases, creates a deb that
# installs to /usr/share/wyoming/models/<name>/
#
set -euo pipefail

MODEL="${1:?Usage: $0 <model-name> (e.g. whisper-tiny.en, whisper-base.en)}"
VERSION="1.0.0"
MAINTAINER="Brian West <brian@kerchunk.net>"
SHERPA_TAG="asr-models"
SHERPA_BASE="https://github.com/k2-fsa/sherpa-onnx/releases/download/$SHERPA_TAG"

# Model catalog
declare -A MODEL_URLS
MODEL_URLS[whisper-tiny.en]="sherpa-onnx-whisper-tiny.en.tar.bz2"
MODEL_URLS[whisper-base.en]="sherpa-onnx-whisper-base.en.tar.bz2"
MODEL_URLS[whisper-small.en]="sherpa-onnx-whisper-small.en.tar.bz2"
MODEL_URLS[whisper-small]="sherpa-onnx-whisper-small.tar.bz2"
MODEL_URLS[whisper-medium.en]="sherpa-onnx-whisper-medium.en.tar.bz2"
MODEL_URLS[zipformer-en]="sherpa-onnx-streaming-zipformer-en-2023-06-26.tar.bz2"

# Model type mapping
declare -A MODEL_TYPES
MODEL_TYPES[whisper-tiny.en]="whisper"
MODEL_TYPES[whisper-base.en]="whisper"
MODEL_TYPES[whisper-small.en]="whisper"
MODEL_TYPES[whisper-small]="whisper"
MODEL_TYPES[whisper-medium.en]="whisper"
MODEL_TYPES[zipformer-en]="zipformer"

# Language mapping
declare -A MODEL_LANGS
MODEL_LANGS[whisper-tiny.en]="en"
MODEL_LANGS[whisper-base.en]="en"
MODEL_LANGS[whisper-small.en]="en"
MODEL_LANGS[whisper-small]="auto"
MODEL_LANGS[whisper-medium.en]="en"
MODEL_LANGS[zipformer-en]="en"

# Streaming flag
declare -A MODEL_STREAMING
MODEL_STREAMING[zipformer-en]="1"

if [ -z "${MODEL_URLS[$MODEL]+x}" ]; then
  echo "Unknown model: $MODEL" >&2
  echo "Available: ${!MODEL_URLS[*]}" >&2
  exit 1
fi

ARCHIVE="${MODEL_URLS[$MODEL]}"
MODEL_TYPE="${MODEL_TYPES[$MODEL]}"
LANGUAGE="${MODEL_LANGS[$MODEL]}"

# Debian package name
PKG_NAME="wyoming-model-$(echo "$MODEL" | tr '.' '-')"

echo "=== Packaging model: $MODEL ==="
echo "  Package: $PKG_NAME"
echo "  Type: $MODEL_TYPE"
echo "  Language: $LANGUAGE"

# Create build directory
BUILD_DIR=$(mktemp -d)
trap "rm -rf $BUILD_DIR" EXIT

# Download and extract
echo "Downloading $ARCHIVE..."
curl -sL "$SHERPA_BASE/$ARCHIVE" | tar xj -C "$BUILD_DIR"

# Find extracted directory (e.g. sherpa-onnx-whisper-tiny.en)
EXTRACTED=$(ls -d "$BUILD_DIR"/sherpa-onnx-* 2>/dev/null | head -1)
if [ -z "$EXTRACTED" ]; then
  echo "ERROR: No extracted directory found" >&2
  exit 1
fi

# Set up install directory
INSTALL_DIR="$BUILD_DIR/pkg/usr/share/wyoming/models/$MODEL"
mkdir -p "$INSTALL_DIR"

# Copy and create standard symlinks for sherpa-onnx
if [ "$MODEL_TYPE" = "whisper" ]; then
  # Whisper models have: tiny.en-encoder.int8.onnx, tiny.en-decoder.int8.onnx, tiny.en-tokens.txt
  # Find the int8 variants (smaller) or fall back to full
  VARIANT=$(echo "$MODEL" | sed 's/whisper-//')

  ENC=$(ls "$EXTRACTED"/${VARIANT}-encoder.int8.onnx 2>/dev/null || ls "$EXTRACTED"/${VARIANT}-encoder.onnx 2>/dev/null)
  DEC=$(ls "$EXTRACTED"/${VARIANT}-decoder.int8.onnx 2>/dev/null || ls "$EXTRACTED"/${VARIANT}-decoder.onnx 2>/dev/null)
  TOK=$(ls "$EXTRACTED"/${VARIANT}-tokens.txt 2>/dev/null)

  if [ -z "$ENC" ] || [ -z "$DEC" ] || [ -z "$TOK" ]; then
    echo "ERROR: Missing model files in $EXTRACTED" >&2
    ls "$EXTRACTED"/ >&2
    exit 1
  fi

  cp "$ENC" "$INSTALL_DIR/encoder.onnx"
  cp "$DEC" "$INSTALL_DIR/decoder.onnx"
  cp "$TOK" "$INSTALL_DIR/tokens.txt"
elif [ "$MODEL_TYPE" = "zipformer" ]; then
  # Zipformer transducer: encoder, decoder, joiner, tokens
  # Prefer int8 variants
  ENC=$(ls "$EXTRACTED"/*encoder*.int8.onnx 2>/dev/null | head -1 || ls "$EXTRACTED"/*encoder*.onnx 2>/dev/null | head -1)
  DEC=$(ls "$EXTRACTED"/*decoder*.int8.onnx 2>/dev/null | head -1 || ls "$EXTRACTED"/*decoder*.onnx 2>/dev/null | head -1)
  JOI=$(ls "$EXTRACTED"/*joiner*.int8.onnx 2>/dev/null | head -1 || ls "$EXTRACTED"/*joiner*.onnx 2>/dev/null | head -1)
  TOK=$(ls "$EXTRACTED"/tokens.txt 2>/dev/null)

  if [ -z "$ENC" ] || [ -z "$DEC" ] || [ -z "$JOI" ] || [ -z "$TOK" ]; then
    echo "ERROR: Missing zipformer model files in $EXTRACTED" >&2
    ls "$EXTRACTED"/ >&2
    exit 1
  fi

  cp "$ENC" "$INSTALL_DIR/encoder.onnx"
  cp "$DEC" "$INSTALL_DIR/decoder.onnx"
  cp "$JOI" "$INSTALL_DIR/joiner.onnx"
  cp "$TOK" "$INSTALL_DIR/tokens.txt"
fi

# Copy test wavs if present
if [ -d "$EXTRACTED/test_wavs" ]; then
  cp -r "$EXTRACTED/test_wavs" "$INSTALL_DIR/"
fi

# Calculate sizes
INSTALLED_SIZE=$(du -sk "$BUILD_DIR/pkg/usr" | awk '{print $1}')
ENC_SIZE=$(stat -c%s "$INSTALL_DIR/encoder.onnx" 2>/dev/null || echo 0)
DEC_SIZE=$(stat -c%s "$INSTALL_DIR/decoder.onnx" 2>/dev/null || echo 0)
TOTAL_MB=$(( (ENC_SIZE + DEC_SIZE) / 1024 / 1024 ))
echo "  Model size: ${TOTAL_MB}MB (encoder + decoder)"

# Build debian package
DEBIAN_DIR="$BUILD_DIR/pkg/DEBIAN"
mkdir -p "$DEBIAN_DIR"

cat > "$DEBIAN_DIR/control" << EOF
Package: $PKG_NAME
Version: $VERSION
Architecture: all
Section: sound
Priority: optional
Maintainer: $MAINTAINER
Depends: wyoming-tools
Installed-Size: $INSTALLED_SIZE
Description: Sherpa-ONNX ASR model: $MODEL
 Speech recognition model for the Wyoming ASR server.
 .
 Model: $MODEL ($MODEL_TYPE)
 Language: $LANGUAGE
 Size: ${TOTAL_MB}MB
 .
 Install path: /usr/share/wyoming/models/$MODEL/
EOF

# Build .deb
DEB_FILE="${PKG_NAME}_${VERSION}_all.deb"
dpkg-deb --build "$BUILD_DIR/pkg" "$DEB_FILE"

STREAMING_FLAG="${MODEL_STREAMING[$MODEL]:-}"
echo "=== Built: $DEB_FILE ==="
echo "  Install: sudo dpkg -i $DEB_FILE"
if [ "$STREAMING_FLAG" = "1" ]; then
  echo "  Use: wyoming-asr-server --model-dir /usr/share/wyoming/models/$MODEL --model-type $MODEL_TYPE --language $LANGUAGE --streaming"
else
  echo "  Use: wyoming-asr-server --model-dir /usr/share/wyoming/models/$MODEL --model-type $MODEL_TYPE --language $LANGUAGE"
fi
# This was already added via the catalog above
