#!/bin/bash
#
# build-all.sh — Build all priority voice and model data packages
#
set -euo pipefail

cd "$(dirname "$0")"

echo "=========================================="
echo "Building Wyoming data packages"
echo "=========================================="

# Priority voices
VOICES=(
  en_US-lessac-high
  en_US-amy-medium
  en_GB-alba-medium
)

# Priority ASR models
MODELS=(
  whisper-tiny.en
  whisper-base.en
)

echo ""
echo "=== Building voice packages ==="
for v in "${VOICES[@]}"; do
  echo ""
  bash package-voice.sh "$v" || echo "WARN: Failed to build $v"
done

echo ""
echo "=== Building ASR model packages ==="
for m in "${MODELS[@]}"; do
  echo ""
  bash package-model.sh "$m" || echo "WARN: Failed to build $m"
done

echo ""
echo "=========================================="
echo "Built packages:"
ls -lh *.deb 2>/dev/null || echo "(none)"
echo ""
echo "Install all: sudo dpkg -i wyoming-voice-*.deb wyoming-model-*.deb"
echo "=========================================="
