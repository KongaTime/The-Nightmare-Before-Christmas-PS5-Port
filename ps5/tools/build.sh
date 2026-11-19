#!/usr/bin/env bash
# Oogie's Revenge PS5 - build, link and sign the title (M4).
#   ps5/tools/build.sh [--clean]
# Needs PS5_SDK (default $HOME/ps5-sdk) and PS5_VULKAN_DIR (default
# $HOME/PS5_Vulkan) for the native tool + runtime module.
# Result: ps5/build/dist/PPSA90030/
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
app=$(dirname "$here")
sdk=${PS5_SDK:-$HOME/ps5-sdk}
vulkan=${PS5_VULKAN_DIR:-$HOME/PS5_Vulkan}
tool=${PS5_NATIVE_TOOL:-$vulkan/build/host/ps5-native-tool}
build=$app/build
fail() { printf 'error: %s\n' "$*" >&2; exit 2; }
[[ -f $sdk/toolchain/prospero.cmake ]] || fail "no PS5 SDK at $sdk (set PS5_SDK)"
[[ -x $tool ]] || fail "no PS5 native tool at $tool (set PS5_NATIVE_TOOL)"
[[ ${1:-} == --clean ]] && rm -rf "$build"
mkdir -p "$build"
cmake=$(command -v cmake || true); [[ -n $cmake ]] || fail "no cmake"
if command -v ninja >/dev/null; then generator=Ninja; else generator="Unix Makefiles"; fi
# M9: Vulkan through RADV (docs/M9-VULKAN-PLAN.md). PS5_Vulkan's release
# archive is linked whole with its own link recipe (tools/radv-link.sh), run
# here against this title's SDK; the inputs and flags go to CMake as a list.
# Without the archive the title builds as before, VideoOut only.
radv_archive=${RADV_ARCHIVE:-$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
radv_args=()
agc_stub_args=()
if [[ -f $radv_archive && -f $vulkan/tools/radv-link.sh && ${OOGIE_NO_VULKAN:-0} != 1 ]]; then
  # shellcheck source=/dev/null
  source "$vulkan/tools/radv-link.sh"
  if radv_link_recipe "$vulkan" "$sdk" "$radv_archive"; then
    flags_file=$build/radv_link_flags.txt
    : > "$flags_file"
    for f in "${radv_link_inputs[@]}" "${radv_link_flags[@]}"; do
      printf '%s\n' "-Wl,$f" >> "$flags_file"
    done
    # Mesa's C11 threads (src/c11/impl/threads_posix.c, in the archive linked
    # whole) and the SDK libc.a's (mtx.o, cnd.o, thrd.o) define the same
    # names: both are thin pthread wrappers, and the title's own calls go
    # through --wrap=mtx_*/thrd_* anyway. The first definition, Mesa's, wins.
    printf '%s\n' "-Wl,--allow-multiple-definition" >> "$flags_file"
    # RADV's winsys imports the AGC driver. Neither SDK has stubs for its two
    # modules, so they are built here from PS5_Vulkan's sources, as its RADV
    # titles do (tools/build-radv-title.sh), and passed to the native link.
    mkdir -p "$build/agc"
    for entry in "libSceAgc agc_canary_link_stub.c" "libSceAgcDriver agc_driver_canary_link_stub.c"; do
      set -- $entry
      "$sdk/bin/prospero-clang" -std=c11 -O2 -fPIC -c "$vulkan/vendor/ps5/sdk/stubs/$2" \
          -o "$build/agc/$1_stub.o"
      "$sdk/bin/prospero-lld" --shared -soname "$1.prx" -o "$build/agc/$1.so" "$build/agc/$1_stub.o"
      printf '%s\n' "$build/agc/$1.so" >> "$flags_file"
      agc_stub_args+=(--stub "$build/agc/$1.so")
    done
    # The render back end's shaders (ps5/shaders), compiled to SPIR-V here
    # and included by platform/ps5_vk.c as uint32_t arrays.
    command -v glslangValidator >/dev/null || fail "glslangValidator not found (apt install glslang-tools)"
    mkdir -p "$build/gen"
    : > "$build/gen/vk_shaders.h.tmp"
    for sh in be.vert be.frag be_vsh.vert present.vert present.frag; do
      glslangValidator -V --target-env vulkan1.3 --vn "spv_${sh//./_}" \
          "$app/shaders/$sh" -o "$build/gen/spv_${sh//./_}.h" >/dev/null \
        || { glslangValidator -V --target-env vulkan1.3 "$app/shaders/$sh" -o /dev/null; fail "shader $sh"; }
      cat "$build/gen/spv_${sh//./_}.h" >> "$build/gen/vk_shaders.h.tmp"
    done
    mv "$build/gen/vk_shaders.h.tmp" "$build/gen/vk_shaders.h"
    radv_args=(-DOOGIE_RADV_FLAGS_FILE="$flags_file"
               -DOOGIE_VULKAN_INCLUDE="$(dirname "$(dirname "$radv_archive")")/include")
    printf '==> linking RADV: %s\n' "$radv_archive"
  else
    printf 'warning: RADV link recipe incomplete; building without Vulkan\n' >&2
  fi
fi
printf '==> configuring (%s)\n' "$generator"
# Restore thread-local SEH for a header already processed by an older build,
# and remove optional indirect-call tracing from the generated header.
gen=${OOGIE_GEN_DIR:-$HOME/oogie-work/gen}
if [[ -f $gen/recomp_types.h ]]; then
  python3 "$here/release_strip.py" "$gen/recomp_types.h"
fi

"$cmake" -S "$app" -B "$build" -G "$generator" \
  -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DOOGIE_TUNE_GUEST="${OOGIE_TUNE_GUEST:-OFF}" \
  -DOOGIE_GEN_DIR="$gen" -DOOGIE_PASS_CONTEXT="${OOGIE_PASS_CONTEXT:-OFF}" \
  -DCMAKE_C_COMPILER="$sdk/bin/prospero-clang" \
  -DCMAKE_CXX_COMPILER="$sdk/bin/prospero-clang++" \
  -UOOGIE_RADV_FLAGS_FILE -UOOGIE_VULKAN_INCLUDE "${radv_args[@]}"
printf '==> compiling\n'
"$cmake" --build "$build" --parallel "$(nproc)"
elf=$build/oogie_ps5; [[ -f $elf ]] || fail "the build produced no $elf"
title_id=$(sed -n 's/.*"titleId": *"\([^"]*\)".*/\1/p' "$app/sce_sys/param.json")
dist=$app/build/dist/$title_id
mkdir -p "$dist/sce_sys" "$dist/sce_module" "$build/link"
printf '==> linking the module layout\n'
"$tool" link --in "$elf" --out "$build/link/eboot.elf" \
  --stub-dir "$sdk/target/lib" ${agc_stub_args[@]+"${agc_stub_args[@]}"} \
  --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
printf '==> signing\n'
"$tool" self --sign --in "$build/link/eboot.elf" --out "$dist/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$dist/eboot.bin" >/dev/null
runtime=${PS5_RUNTIME_MODULE:-$vulkan/runtime/libc.prx}
[[ -f $runtime ]] || fail "no runtime module at $runtime (run 'make libc' in $vulkan)"
cp "$runtime" "$dist/sce_module/libc.prx"
"$tool" self --inspect --file "$dist/sce_module/libc.prx" >/dev/null
# The console serves a signed module back decrypted; this is the form
# tools/install.py --verify compares a read-back against.
"$tool" self --extract --file "$dist/sce_module/libc.prx" --out "$build/link/libc.elf" >/dev/null
cp "$app/sce_sys/param.json" "$dist/sce_sys/param.json"
for asset in icon0.png pic0.dds pic1.dds; do
  [[ ! -f $app/sce_sys/$asset ]] || cp "$app/sce_sys/$asset" "$dist/sce_sys/$asset"
done
if [[ ! -f $app/title/assets/setup-menu.bin || ! -f $app/sce_sys/icon0.png ]]; then
  art_dir=$build/launcher-art
  python3 "$here/prepare_launcher_art.py" "$art_dir"
fi
if [[ -f $app/title/assets/setup-menu.bin ]]; then
  cp "$app/title/assets/setup-menu.bin" "$dist/setup-menu.bin"
else
  cp "$art_dir/setup-menu.bin" "$dist/setup-menu.bin"
fi
if [[ ! -f $app/sce_sys/icon0.png ]]; then
  cp "$art_dir/sce_sys/icon0.png" "$dist/sce_sys/icon0.png"
fi
mkdir -p "$dist/Oogie"
# Trapped MMIO (platform/ps5_mmio.c) decodes the faulting instruction, and the
# console's code pages are execute-only: ship a readable copy of the text
# segment plus where one known function sits in it.
anchor=$("$sdk/bin/llvm-nm" "$elf" | awk '$3=="oogie_text_mark"{print $1}')
[[ -n $anchor ]] || fail "no oogie_text_mark symbol in $elf"
python3 - "$build/link/eboot.elf" "$dist/oogie_code.bin" "$dist/oogie_code.map" "$anchor" <<'PY'
import struct, sys
src, out_bin, out_map, anchor = sys.argv[1:5]
d = open(src, 'rb').read()
phoff, = struct.unpack_from('<Q', d, 0x20)
phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
for i in range(phnum):
    t, fl, off, va, pa, fs, ms, al = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize)
    if t == 1 and va == 0 and (fl & 1):
        open(out_bin, 'wb').write(d[off:off + fs])
        open(out_map, 'w').write('anchor=%s size=%x\n' % (anchor, fs))
        print('code copy: %d bytes from offset 0x%x, anchor 0x%s' % (fs, off, anchor))
        break
else:
    sys.exit('no executable PT_LOAD at vaddr 0')
PY
printf '==> %s: %s (eboot.bin %s bytes)\n' "$title_id" "$dist" "$(stat -c %s "$dist/eboot.bin")"
