# Cronopio Quake

A port of **Quake** (id Software's software-rendered 1996 engine) to the
**Cronopio** fantasy console / CronoVM virtual machine — the sibling project to
[cronopio-doom](../cronopio-doom). DOOM proved the platform; Quake exercises its
3D, floating-point and performance limits, and the port doubles as a driver for
**completing whatever Cronopio is still missing**.

## Engine base

Upstream is [Chocolate Quake](https://github.com/Henrique194/chocolate-quake)
(Henrique194) — a purist, C99, software-rendered Quake (no x86 asm, SDL2 host).
We track a fork at `cronomantic/chocolate-quake`, branch **`cronopio-port`**,
where the `[cronopio]` engine patches live (never pushed upstream). Both the
engine and the Cronopio SDK are git submodules:

```
third_party/Cronopio          cronomantic/Cronopio       (nested: CronoVM, TinySoundFont)
third_party/chocolate-quake   cronomantic/chocolate-quake (branch cronopio-port)
```

## Architecture (mirrors cronopio-doom)

The upstream engine is C; only its **platform subsystems** are replaced with
Cronopio implementations in `src/` (`*_cron.c`), the rest is compiled as-is and
linked by `cvm-cc`. The PAK is baked into cartridge ROM via `--rom`.

| Upstream subsystem | Replaced by | Notes |
|---|---|---|
| `sys/sys.c` | `src/sys_cron.c` | time (virtual clock), args, exit, file IO over the libc RAM-FS |
| `video/vid_*` | `src/vid_cron.c` | software framebuffer → `cron_fb`, palette → `cron_pal` |
| `input/in_{gamepad,keyboard,mouse}` | `src/in_cron.c` | 12-button Cronopio pad → Quake commands (`in_main`, `keys` kept) |
| `sound/{snd_sdl,codecs}` | `src/snd_cron.c` | SFX via `cron_pcm`; flac/mp3/vorbis codecs dropped |
| `net` sockets | `src/net_stub_cron.c` | single-player only (`net_loop` kept), rest stubbed |
| `main.c` | `src/engine_cron.c` | Cronopio entry + frame callback |

**Kept (engine logic):** camera, client, cmd, common, console, crc, host,
mathlib, memory, menu, model, progs, renderer (all `d_*`/`r_*`), screen, server,
status_bar, wad.

## Build / run

```
bash build_quake.sh [PAK] [out.crom]   # → quake.crom (cronopio-cc links + seals the cart)
third_party/Cronopio/build/host/desktop/cronopio.exe quake.crom
```

`build_quake.sh` resolves the repo root from its own path, inits the Cronopio
submodule if missing, and auto-builds the SDK tools on first run (CMake+Ninja+clang).

## Milestones

- **M0** Scaffold: submodules, build script, platform-seam stubs → `quake.crom` compiles.
- **M1** Boot to the Quake console/menu on the Cronopio framebuffer (PAK in ROM, file IO over RAM-FS).
- **M2** Software renderer to the framebuffer (BSP world; look around). Float-heavy → drives CronoVM perf work.
- **M3** Input (12-button pad) + game loop + console/menu nav.
- **M4** Sound (SFX via `cron_pcm`; music TBD).
- **M5** Cartridge packaging (`.crom`, metadata, seal, launcher).
- **Later** Offload the renderer to `cron_polys` (perspective-correct host rasteriser) — "use Cronopio's 3D".

See `TODO.txt` for the live backlog.
