#!/usr/bin/env bash
set -euo pipefail

# The release archive is checksum-pinned; only the ZipDepth ONNX file is kept.
archive_sha=d711303859905e2722ce86ea33d99ed84ce58fd3b0ce46e1f26c0e388bb50f67
model_sha=614925332b4f4ade6460279609b94a93eeae42fc1cd4be29283b0b51334acf61
release_url=https://github.com/pkellyuk/VRX/releases/download/v1.7.6/VRX-1.7.6-win-x64.zip
model_inside=VRX-1.7.6-win-x64/engine/models/zipdepth_faithful_fp16_672x384.onnx
repo_root=$(cd "$(dirname "$0")/.." && pwd)
destination="$repo_root/bench/models/zipdepth_faithful_fp16_672x384.onnx"

if [[ -f "$destination" ]] && printf '%s  %s\n' "$model_sha" "$destination" | sha256sum --check --status; then
    printf 'ZipDepth model already verified: %s\n' "$destination"
    exit 0
fi

archive=$(mktemp)
partial=$(mktemp)
trap 'rm -f "$archive" "$partial"' EXIT
curl --fail --location --retry 3 --output "$archive" "$release_url"
printf '%s  %s\n' "$archive_sha" "$archive" | sha256sum --check
unzip -p "$archive" "$model_inside" > "$partial"
printf '%s  %s\n' "$model_sha" "$partial" | sha256sum --check
mkdir -p "$(dirname "$destination")"
mv "$partial" "$destination"
printf 'ZipDepth model ready: %s\n' "$destination"
