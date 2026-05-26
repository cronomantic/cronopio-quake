#!/usr/bin/env bash
# Build the Cronopio Quake cartridge (Chocolate Quake -> Cronopio .crom).
#
# Multi-file build: cronopio-cc compiles every .c to bitcode, llvm-links them,
# translates, and seals the result into quake.crom (magic + crc + metadata —
# the host launcher only lists sealed .crom carts). The PAK is baked into ROM
# via --rom.
#
# Mirrors cronopio-doom/build_doom.sh. The KEEP lists below are the upstream
# engine TUs we compile as-is; the platform subsystems (sys/vid/in/snd/net) are
# replaced by our src/*_cron.c. Expect the KEEP lists to need tuning as the
# translator surfaces unsupported constructs (that's the point — it drives
# completing Cronopio).
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QS="$ROOT/third_party/chocolate-quake/src"

# Cronopio SDK submodule (nested CronoVM + TinySoundFont). Init if missing.
CRONOPIO="$ROOT/third_party/Cronopio"
if [[ ! -f "$CRONOPIO/CMakeLists.txt" ]]; then
  echo "[build] Cronopio submodule missing — initialising..."
  git -C "$ROOT" submodule update --init --recursive third_party/Cronopio || {
    echo "[build] ERROR: could not init the Cronopio submodule." >&2; exit 1; }
fi

SDK="$CRONOPIO/sdk"
RT="$CRONOPIO/external/CronoVM/runtime/lib"
CRBUILD="$CRONOPIO/build"
CC="$CRBUILD/tools/cronopio-cc/cronopio-cc.exe"

# The cart compiler + the host/headless are built artifacts. ALWAYS run an
# incremental ninja build (a no-op when nothing changed, ~instant) so the host
# and headless track the current CronoVM — a stale host links an older VM and
# traps "unknown opcode" on a freshly-built cart (the launch hang we hit once).
if [[ ! -f "$CRBUILD/build.ninja" ]]; then
  echo "[build] configuring Cronopio SDK (one-time)..."
  cmake -S "$CRONOPIO" -B "$CRBUILD" -G Ninja || {
    echo "[build] ERROR: cmake configure of Cronopio failed." >&2; exit 1; }
fi
echo "[build] syncing Cronopio tools + host with the current VM..."
ninja -C "$CRBUILD" cronopio-cc cronopio cronopio-headless || {
  echo "[build] ERROR: building Cronopio tools failed." >&2; exit 1; }

OUT="${2:-$ROOT/quake.crom}"

# --- PAK source ------------------------------------------------------------
# An explicit PAK arg ($1) is baked as-is. Otherwise auto-discover
# basegame/id1/pak0.pak, pak1.pak, … (case-insensitive) in load order: a single
# pak is baked directly; several are MERGED into one (cron_rom is a single blob,
# so multi-pak = one merged pak served as pak0.pak). Later paks win on duplicate
# names, matching Quake's search-path precedence. The merge is cached and only
# rebuilt when an input pak (or pakmerge.c) is newer.
PAKDIR="$ROOT/basegame/id1"
if [[ -n "${1:-}" ]]; then
  PAK="$1"
else
  # Discover pak0,pak1,… in load order, stopping at the first gap (as Quake
  # does). Test the lower- and upper-case spellings explicitly — nullglob is
  # unreliable under git-bash.
  DISC=()
  for i in 0 1 2 3 4 5 6 7 8 9; do
    f=""
    for c in "$PAKDIR/pak$i.pak" "$PAKDIR/PAK$i.PAK"; do
      [[ -f "$c" ]] && { f="$c"; break; }
    done
    [[ -z "$f" ]] && break
    DISC+=( "$f" )
  done

  if [[ ${#DISC[@]} -le 1 ]]; then
    PAK="${DISC[0]:-$PAKDIR/pak0.pak}"
  else
    HOSTCC="${HOSTCC:-cc}"
    PAKMERGE="$ROOT/tools/pakmerge.exe"
    MERGED="$ROOT/build/quake_merged.pak"
    mkdir -p "$ROOT/build"
    if [[ ! -x "$PAKMERGE" || "$ROOT/tools/pakmerge.c" -nt "$PAKMERGE" ]]; then
      echo "[build] compiling tools/pakmerge.c ..."
      "$HOSTCC" -O2 -o "$PAKMERGE" "$ROOT/tools/pakmerge.c" || {
        echo "[build] ERROR: could not build pakmerge." >&2; exit 1; }
    fi
    need=0
    [[ -f "$MERGED" ]] || need=1
    for p in "${DISC[@]}"; do [[ "$p" -nt "$MERGED" ]] && need=1; done
    if [[ $need -eq 1 ]]; then
      echo "[build] merging ${#DISC[@]} paks -> $MERGED"
      "$PAKMERGE" "$MERGED" "${DISC[@]}" || {
        echo "[build] ERROR: pak merge failed." >&2; exit 1; }
    fi
    PAK="$MERGED"
  fi
fi

# --- include dirs: our compat/src + every chocolate-quake subsystem include --
INCS=( -I "$ROOT/compat" -I "$ROOT/src" -I "$SDK/include" -I "$RT" )
for d in "$QS"/*/include; do INCS+=( -I "$d" ); done

# --- upstream engine TUs we keep (subsystem:file ...) -----------------------
# Platform subsystems (sys, video/vid_*, input/in_{gamepad,keyboard,mouse},
# sound SDL+codecs, net sockets, main.c) are intentionally NOT here — replaced
# by src/*_cron.c below.
KEEP=(
  camera/chase camera/view
  client/cl_demo client/cl_input client/cl_main client/cl_parse client/cl_tent
  cmd/cmd
  common/com_argv common/com_byte common/com_ext common/com_fs common/com_init
  common/com_link common/com_msg common/com_sizebuf common/com_string
  common/com_token common/com_va common/com_stdlib common/com_stdio
  console/console console/cvar
  crc/crc
  host/host host/host_cmd
  input/keys
  mathlib/mathlib
  memory/zone
  menu/menu
  model/model
  net/net_main net/net_loop net/net_poll net/net_socket net/net_vcr
  progs/pr_cmds progs/pr_edict progs/pr_exec
  renderer/draw renderer/d_edge renderer/d_fill renderer/d_init renderer/d_modech
  renderer/d_part renderer/d_polyse renderer/d_scan renderer/d_sky renderer/d_sprite
  renderer/d_surf renderer/d_vars renderer/d_zpoint renderer/nonintel
  renderer/r_aclip renderer/r_alias renderer/r_bsp renderer/r_draw renderer/r_edge
  renderer/r_efrag renderer/r_light renderer/r_main renderer/r_misc renderer/r_part
  renderer/r_sky renderer/r_sprite renderer/r_surf renderer/r_vars
  screen/screen
  server/sv_main server/sv_move server/sv_phys server/sv_user server/sv_world
  status_bar/sbar
  wad/wad
)

SOURCES=()
for f in "${KEEP[@]}"; do
  sub="${f%%/*}"; file="${f##*/}"
  SOURCES+=( "$QS/$sub/src/$file.c" )
done

# --- our port-local platform seam (src/*) -----------------------------------
PORT=(
  "$ROOT/src/engine_cron.c"     # entry + frame callback (replaces main.c)
  "$ROOT/src/sys_cron.c"        # time, args, exit, file IO over libc RAM-FS
  "$ROOT/src/vid_cron.c"        # software framebuffer -> cron_fb + palette
  "$ROOT/src/in_cron.c"         # 12-button pad -> Quake commands
  "$ROOT/src/snd_cron.c"        # SFX via cron_pcm (codecs dropped)
  "$ROOT/src/net_stub_cron.c"   # single-player: net_loop kept, rest stubbed
  "$SDK/lib/cvm_libc.c"
)

echo "[build] $(( ${#SOURCES[@]} + ${#PORT[@]} )) translation units -> $OUT"
echo "[build] PAK: $PAK"

# Optional cartridge metadata (CVM_SEC_META): the host launcher shows these.
META_ARGS=()
[[ -n "${CART_TITLE:-}"    ]] && META_ARGS+=( --title="$CART_TITLE" )
[[ -n "${CART_AUTHOR:-}"   ]] && META_ARGS+=( --author="$CART_AUTHOR" )
[[ -n "${CART_CONTROLS:-}" ]] && META_ARGS+=( --controls="$CART_CONTROLS" )

ROM_ARGS=()
[[ -f "$PAK" ]] && ROM_ARGS+=( --rom="$PAK" )

# Quake uses doubles (time, entity parsing), so it's an f64 cart already — pull
# in the libc's real f64 atof/strtod (guarded off by default to keep f64-free
# carts like DOOM clean).
"$CC" \
  -DCVM_LIBC_ENABLE_F64 \
  "${INCS[@]}" \
  "${SOURCES[@]}" \
  "${PORT[@]}" \
  ${ROM_ARGS[@]+"${ROM_ARGS[@]}"} \
  --heap-reserve=96M \
  --stack-reserve=4M \
  ${META_ARGS[@]+"${META_ARGS[@]}"} \
  -o "$OUT"
