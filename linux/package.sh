#!/usr/bin/env bash
# Builds the portable Linux package: build/linux-package/vrx-linux-<version>[-cuda]-x86_64.tar.gz.
#
#   linux/package.sh                     small package; depth uses the PC's CUDA 12 and cuDNN 9
#   linux/package.sh --with-cuda[=DIR]   also bundles CUDA 12 and cuDNN 9 (about 3 GB unpacked),
#                                        taken from NVIDIA's pip wheels in DIR (or $VRX_CUDA_WHEELS)
#
# Other options: --build-dir=DIR (default build/linux-release, already configured
# with CMake), --ort=DIR (the ONNX Runtime GPU release; default: the one the build
# links), --output=DIR (default build/linux-package), --version=TEXT (default: git describe).
# CMAKE and DOTNET name the tools when they are not on the PATH.
#
# The CUDA wheels can be fetched with:
#   pip download --no-deps -d DIR nvidia-cuda-runtime-cu12 nvidia-cublas-cu12 \
#       nvidia-cufft-cu12 nvidia-curand-cu12 'nvidia-cudnn-cu12>=9,<10'
set -euo pipefail

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build="$repo/build/linux-release"
output="$repo/build/linux-package"
ort=""
cuda_wheels=""
with_cuda=0
version=""
for argument in "$@"; do
    case "$argument" in
        --with-cuda) with_cuda=1; cuda_wheels="${VRX_CUDA_WHEELS:-}" ;;
        --with-cuda=*) with_cuda=1; cuda_wheels="${argument#*=}" ;;
        --build-dir=*) build=$(realpath "${argument#*=}") ;;
        --ort=*) ort=$(realpath "${argument#*=}") ;;
        --output=*) output=$(realpath -m "${argument#*=}") ;;
        --version=*) version="${argument#*=}" ;;
        --help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "package.sh: unknown option $argument (see --help)" >&2; exit 2 ;;
    esac
done

fail() { echo "package.sh: $*" >&2; exit 1; }
cmake=${CMAKE:-cmake}
command -v "$cmake" >/dev/null || fail "cmake not found; set CMAKE"
dotnet=${DOTNET:-$(command -v dotnet || echo "$HOME/.dotnet/dotnet")}
[[ -x "$dotnet" ]] || fail ".NET SDK not found; set DOTNET"
[[ -f "$build/CMakeCache.txt" ]] || fail "$build is not a configured CMake build"
if [[ -z "$ort" ]]; then
    ort_library=$(sed -n 's/^ORT_LIBRARY:FILEPATH=//p' "$build/CMakeCache.txt")
    [[ -n "$ort_library" ]] || fail "the build has no ONNX Runtime; configure it with ONNX Runtime GPU"
    ort=$(dirname "$(dirname "$(realpath "$ort_library")")")
fi
[[ -f "$ort/lib/libonnxruntime_providers_cuda.so" ]] || fail "$ort is not an ONNX Runtime GPU release"
if (( with_cuda )); then
    [[ -n "$cuda_wheels" && -d "$cuda_wheels" ]] || fail "--with-cuda needs a directory of NVIDIA wheels (see --help)"
fi
[[ -n "$version" ]] || version=$(git -C "$repo" describe --tags --always --dirty 2>/dev/null | sed 's/^v//')
[[ -n "$version" ]] || version=unknown

suffix=""
(( with_cuda )) && suffix=-cuda
name="vrx-linux-$version$suffix-x86_64"
stage="$output/$name"
rm -rf "$stage"
mkdir -p "$stage"/{lib,licenses,share/vrx/models,share/vrx/icons}

echo "== Engine ($build)"
"$cmake" --build "$build" --target vrx-engine vrx-probe
"$cmake" --install "$build" --prefix "$stage" >/dev/null
[[ -x "$stage/bin/vrx-engine" ]] || fail "the engine was not installed"

echo "== Desktop app (self-contained)"
DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 "$dotnet" publish "$repo/linux/VRX.Linux" -c Release \
    -r linux-x64 --self-contained -p:DebugType=none -v quiet --nologo -o "$stage/app"
[[ -x "$stage/app/vrx-linux" ]] || fail "the desktop app was not published"

echo "== ONNX Runtime $(cat "$ort/VERSION_NUMBER" 2>/dev/null) ($ort)"
cp -P "$ort"/lib/libonnxruntime.so* "$ort/lib/libonnxruntime_providers_cuda.so" \
      "$ort/lib/libonnxruntime_providers_shared.so" "$stage/lib/"
cp "$ort/LICENSE" "$stage/licenses/ONNXRuntime-LICENSE.txt"
cp "$ort/ThirdPartyNotices.txt" "$stage/licenses/ONNXRuntime-ThirdPartyNotices.txt"

cuda_notice=""
if (( with_cuda )); then
    echo "== CUDA 12 and cuDNN 9 ($cuda_wheels)"
    unpack=$(mktemp -d)
    trap 'rm -rf "$unpack"' EXIT
    for package in cuda_runtime cublas cufft curand cudnn; do
        wheel=$(ls "$cuda_wheels"/nvidia_${package}_cu12-*.whl 2>/dev/null | sort -V | tail -n 1)
        [[ -n "$wheel" ]] || fail "no nvidia_${package}_cu12 wheel in $cuda_wheels"
        unzip -q -o "$wheel" -d "$unpack/$package"
        wheel_version=$(basename "$wheel" | cut -d- -f2)
        find "$unpack/$package" -name 'License.txt' -path '*dist-info*' -exec cp {} \
            "$stage/licenses/NVIDIA-${package}-${wheel_version}-License.txt" \;
        cuda_notice+="  nvidia-${package//_/-}-cu12 $wheel_version"$'\n'
    done
    for library in libcudart.so.12 libcublas.so.12 libcublasLt.so.12 libcufft.so.11 libcurand.so.10; do
        found=$(find "$unpack" -name "$library" -print -quit)
        [[ -n "$found" ]] || fail "$library is not in the wheels"
        cp "$found" "$stage/lib/"
    done
    find "$unpack/cudnn" -name 'libcudnn*.so.9' -exec cp {} "$stage/lib/" \;
    missing=$(LD_LIBRARY_PATH="$stage/lib" ldd "$stage/lib/libonnxruntime_providers_cuda.so" | awk '/not found/ { print $1 }')
    [[ -z "$missing" ]] || fail "the bundled CUDA provider still needs: $missing"
fi

echo "== Model"
model=zipdepth_faithful_fp16_672x384.onnx
model_sha=$(sed -n 's/^model_sha=//p' "$repo/linux/fetch-model.sh")
[[ -f "$repo/bench/models/$model" ]] || "$repo/linux/fetch-model.sh"
cp "$repo/bench/models/$model" "$stage/share/vrx/models/"
printf '%s  %s\n' "$model_sha" "$model" > "$stage/share/vrx/models/SHA256SUMS"
(cd "$stage/share/vrx/models" && sha256sum --check --quiet SHA256SUMS) || fail "the model's checksum does not match"
cp "$repo/release/licenses/ZipDepth-LICENSE.txt" "$stage/licenses/"

echo "== Launcher, icons and notices"
cp "$repo/linux/package/vrx" "$repo/linux/install-desktop.sh" "$stage/"
cp "$repo/linux/VRX.Linux/Assets"/vrx-*.png "$stage/share/vrx/icons/"
cp "$repo/LICENSE" "$stage/LICENSE.txt"
cp "$repo/linux/package/README.txt" "$stage/README.txt"
echo "$version" > "$stage/VERSION"
runtime_pack=$(ls -d "$HOME"/.nuget/packages/microsoft.netcore.app.runtime.linux-x64/* 2>/dev/null | sort -V | tail -n 1)
dotnet_licenses=${runtime_pack:-$(dirname "$(realpath "$dotnet")")}
for file in LICENSE.TXT LICENSE.txt THIRD-PARTY-NOTICES.TXT ThirdPartyNotices.txt; do
    if [[ -f "$dotnet_licenses/$file" ]]; then cp "$dotnet_licenses/$file" "$stage/licenses/Microsoft.NETCore.App-$file"; fi
done
# Every NuGet package the app ships: its own licence file when it has one, else
# the MIT terms its nuspec declares with its stated copyright.
python3 - "$stage/app/vrx-linux.deps.json" "$HOME/.nuget/packages" "$stage/licenses" <<'PYTHON'
import json, os, re, shutil, sys
deps, packages, licenses = sys.argv[1:]
MIT = """Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""
listing = []
for key, library in json.load(open(deps))["libraries"].items():
    if library.get("type") != "package": continue
    name, version = key.split("/")
    directory = os.path.join(packages, name.lower(), version)
    nuspec = open(os.path.join(directory, name.lower() + ".nuspec"), encoding="utf-8-sig").read()
    expression = re.search(r'<license type="expression">([^<]+)', nuspec)
    copyright_ = re.search(r'<copyright>([^<]+)', nuspec)
    expression = expression.group(1) if expression else "see package"
    listing.append(f"  {name} {version} ({expression})")
    copied = False
    for file in sorted(os.listdir(directory)):
        if re.match(r'(?i)(license|third-party-notices)', file):
            shutil.copy(os.path.join(directory, file), os.path.join(licenses, f"{name}-{file}"))
            copied = True
    if not copied:
        if expression != "MIT": sys.exit(f"{name} {version}: no licence file and not MIT; add its terms by hand")
        with open(os.path.join(licenses, f"{name}-LICENSE.txt"), "w") as out:
            out.write(f"{name} {version}\nMIT License\n\n{copyright_.group(1).strip() if copyright_ else ''}\n\n{MIT}")
open(os.path.join(licenses, "NuGet-packages.txt"), "w").write("NuGet packages in app/:\n" + "\n".join(listing) + "\n")
PYTHON
{
    sed "s/@VERSION@/$version/" "$repo/linux/package/THIRD-PARTY-NOTICES.txt"
    if (( with_cuda )); then
        echo
        echo "NVIDIA CUDA 12 runtime libraries and cuDNN 9 (bundled in lib/ in this -cuda package)"
        echo "https://developer.nvidia.com/cuda-toolkit  https://developer.nvidia.com/cudnn"
        echo "Redistributed unmodified from NVIDIA's pip wheels:"
        printf '%s' "$cuda_notice"
        echo "Under the NVIDIA licence agreements in licenses/NVIDIA-*-License.txt."
    fi
} > "$stage/THIRD-PARTY-NOTICES.txt"

{
    echo "VRX for Linux $version, x86_64"
    echo "Built $(date -u +%Y-%m-%dT%H:%M:%SZ) from $(git -C "$repo" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "ONNX Runtime GPU $(cat "$ort/VERSION_NUMBER" 2>/dev/null)"
    echo ".NET runtime ${runtime_pack##*/} (self-contained; SDK $("$dotnet" --version))"
    if (( with_cuda )); then printf 'Bundled CUDA/cuDNN wheels:\n%s' "$cuda_notice"; fi
} > "$stage/BUILD.txt"
chmod -R u=rwX,go=rX "$stage/licenses" "$stage/share"
(cd "$stage" && find . -type f ! -name FILES.sha256.txt -print0 | sort -z | xargs -0 sha256sum > FILES.sha256.txt)

echo "== Archive"
archive="$output/$name.tar.gz"
compressor=$(command -v pigz || echo gzip)
tar -C "$output" -cf - "$name" | "$compressor" -6 > "$archive.partial"
mv "$archive.partial" "$archive"
(cd "$output" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
echo "Package: $archive ($(du -h "$archive" | cut -f1); $(du -sh "$stage" | cut -f1) unpacked)"
