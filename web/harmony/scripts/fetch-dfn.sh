#!/usr/bin/env bash
# Fetches the two large files the DeepFilterNet 3 noise suppressor needs into
# web/harmony/vendor/dfn (not in git) and checks each against its pinned
# checksum. Run once after cloning, before `npm run build:harmony`.
#
# - DeepFilterNet3_onnx.tar.gz: the official model, from Rikorose/DeepFilterNet
#   (MIT OR Apache-2.0). 7.6 MB.
# - df_bg.wasm: libDF (the official DeepFilterNet runtime, Rust + tract)
#   compiled to WebAssembly by mezonai/mezon-noise-suppression (Apache-2.0),
#   as shipped with deepfilternet3-noise-filter@1.3.0. 15.7 MB. It is paired
#   with the glue in src/dfn/worklet.js; a new wasm needs the glue from the same
#   release.
set -euo pipefail
MODEL_SHA256=c94d91f70911001c946e0fabb4aa9adc37045f45a03b56008cb0c8244cb63616
WASM_SHA256=440b5d12b6ea7d95008736f844221d7874ee15de5cb10d3015002470fdba0432
MODEL_URL=https://github.com/Rikorose/DeepFilterNet/raw/main/models/DeepFilterNet3_onnx.tar.gz
WASM_URL=https://cdn.mezon.ai/AI/models/datas/noise_suppression/deepfilternet3/v3/pkg/df_bg.wasm

mkdir -p "$(dirname "$0")/../vendor/dfn"
cd "$(dirname "$0")/../vendor/dfn"

fetch() {
  local out=$1 sha=$2 url=$3
  if [ -f "$out" ] && echo "$sha  $out" | sha256sum -c --status; then
    echo "$out already present"; return
  fi
  curl -fsSL -o "$out.part" "$url"
  echo "$sha  $out.part" | sha256sum -c --status || { echo "checksum mismatch: $out" >&2; rm -f "$out.part"; exit 1; }
  mv "$out.part" "$out"
  echo "fetched $out"
}

fetch DeepFilterNet3_onnx.tar.gz "$MODEL_SHA256" "$MODEL_URL"
fetch df_bg.wasm "$WASM_SHA256" "$WASM_URL"
