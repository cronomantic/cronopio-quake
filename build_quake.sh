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

# Build picolibc.bc — the C library. Quake keeps picolibc's canonical malloc
# (no --no-malloc); cron_sys.c supplies the sbrk machine port + errno.
echo "[build] building picolibc.bc (C library)..."
bash "$RT/build_picolibc.sh" || {
  echo "[build] ERROR: build_picolibc.sh failed." >&2; exit 1; }

OUT="${2:-$ROOT/quake.crom}"

# --- PAK source ------------------------------------------------------------
# An explicit PAK arg ($1) is baked as-is. Otherwise auto-discover
# basegame/id1/pak0.pak, pak1.pak, … (case-insensitive) in load order, plus a
# pak built from any basegame/id1/music/*.ogg soundtrack. A single pak is baked
# directly; several are MERGED into one (cron_rom is a single blob, so multi-pak
# = one merged pak served as pak0.pak). Later paks win on duplicate names,
# matching Quake's search-path precedence. Tools (pakmerge/pakbuild) are
# compiled on first use; outputs are cached and rebuilt only when an input is
# newer. NOTE: music plays through the host OGG decoder (cron_music); drop
# track02.ogg … track11.ogg under basegame/id1/music/ to include the soundtrack.
PAKDIR="$ROOT/basegame/id1"
HOSTCC="${HOSTCC:-cc}"
mkdir -p "$ROOT/build"
if [[ -n "${1:-}" ]]; then
  PAK="$1"
else
  # Discover pak0,pak1,… in load order, stopping at the first gap (as Quake
  # does). Test the lower- and upper-case spellings explicitly — nullglob is
  # unreliable under git-bash.
  PAKS=()
  for i in 0 1 2 3 4 5 6 7 8 9; do
    f=""
    for c in "$PAKDIR/pak$i.pak" "$PAKDIR/PAK$i.PAK"; do
      [[ -f "$c" ]] && { f="$c"; break; }
    done
    [[ -z "$f" ]] && break
    PAKS+=( "$f" )
  done

  # Pack the soundtrack (basegame/id1/music/*.ogg) into its own pak, to be
  # merged like any other. (Skipped silently when there are no oggs.)
  MUSIC=()
  for f in "$PAKDIR"/music/*.ogg "$PAKDIR"/music/*.OGG; do
    [[ -f "$f" ]] && MUSIC+=( "$f" )
  done
  if [[ ${#MUSIC[@]} -gt 0 ]]; then
    PAKBUILD="$ROOT/tools/pakbuild.exe"
    MUSICPAK="$ROOT/build/quake_music.pak"
    if [[ ! -x "$PAKBUILD" || "$ROOT/tools/pakbuild.c" -nt "$PAKBUILD" ]]; then
      echo "[build] compiling tools/pakbuild.c ..."
      "$HOSTCC" -O2 -o "$PAKBUILD" "$ROOT/tools/pakbuild.c" || {
        echo "[build] ERROR: could not build pakbuild." >&2; exit 1; }
    fi
    need=0
    [[ -f "$MUSICPAK" ]] || need=1
    # The music dir's mtime changes when a track is added/removed; check it too,
    # since a freshly-copied track may keep an OLD mtime (so a per-file -nt test
    # alone would miss it) — this avoids a stale music pak.
    [[ "$PAKDIR/music" -nt "$MUSICPAK" ]] && need=1
    for m in "${MUSIC[@]}"; do [[ "$m" -nt "$MUSICPAK" ]] && need=1; done
    if [[ $need -eq 1 ]]; then
      echo "[build] packing ${#MUSIC[@]} music track(s) -> $MUSICPAK"
      "$PAKBUILD" "$MUSICPAK" "$PAKDIR" "${MUSIC[@]}" || {
        echo "[build] ERROR: music pack failed." >&2; exit 1; }
    fi
    PAKS+=( "$MUSICPAK" )
  fi

  if [[ ${#PAKS[@]} -le 1 ]]; then
    PAK="${PAKS[0]:-$PAKDIR/pak0.pak}"
  else
    PAKMERGE="$ROOT/tools/pakmerge.exe"
    MERGED="$ROOT/build/quake_merged.pak"
    if [[ ! -x "$PAKMERGE" || "$ROOT/tools/pakmerge.c" -nt "$PAKMERGE" ]]; then
      echo "[build] compiling tools/pakmerge.c ..."
      "$HOSTCC" -O2 -o "$PAKMERGE" "$ROOT/tools/pakmerge.c" || {
        echo "[build] ERROR: could not build pakmerge." >&2; exit 1; }
    fi
    need=0
    [[ -f "$MERGED" ]] || need=1
    for p in "${PAKS[@]}"; do [[ "$p" -nt "$MERGED" ]] && need=1; done
    [[ "$ROOT/tools/pakmerge.c" -nt "$MERGED" ]] && need=1
    if [[ $need -eq 1 ]]; then
      echo "[build] merging ${#PAKS[@]} paks -> $MERGED"
      "$PAKMERGE" "$MERGED" "${PAKS[@]}" || {
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
# the sound platform layer snd_sdl + the music codecs, net sockets, main.c) are
# intentionally NOT here — replaced by src/*_cron.c below. NOTE we DO keep
# Quake's own software mixer (sound/snd_dma+snd_mix+snd_mem); snd_cron.c only
# replaces the DMA layer, pushing the mixed stream to the host via cron_stream.
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
  sound/snd_dma sound/snd_mix sound/snd_mem
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
  "$ROOT/src/r_accel_cron.c"    # accelerated 3D path (cron_polys); r_accel toggles it
  "$ROOT/src/snd_cron.c"        # SFX via cron_pcm (codecs dropped)
  "$ROOT/src/net_stub_cron.c"   # single-player: net_loop kept, rest stubbed
  "$SDK/lib/cron_sys.c"
  "$RT/picolibc.bc"
)

echo "[build] $(( ${#SOURCES[@]} + ${#PORT[@]} )) translation units -> $OUT"
echo "[build] PAK: $PAK"

# Cartridge metadata (CVM_SEC_META): the host launcher shows these without
# running the cart. Defaults baked in so the browser shows more than bare
# "quake"; override via the CART_* env vars.
CART_TITLE="${CART_TITLE:-Quake}"
CART_AUTHOR="${CART_AUTHOR:-id Software (Cronopio port)}"
CART_CONTROLS="${CART_CONTROLS:-D-pad: move/turn  L/R: strafe  A: fire  B: jump  X/Y: weapon  START: menu  SELECT: console}"
META_ARGS=( --title="$CART_TITLE" --author="$CART_AUTHOR" --controls="$CART_CONTROLS" )

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
