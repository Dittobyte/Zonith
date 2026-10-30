# HANDOFF: TotK Recomp on Linux/Proton (Legolas, Deck), updated end of 2026-10-30

Read this, then `read_chat` on the hub for anything after #45. Today's full changelog with numbers: `~/Games/TotK-Recomp/labtools/CHANGELOG-2026-10-30-deck.md` (also published for Josh as the artifact "TotK Proton Day Log, Oct 30").

## Roles
- **Legolas** (this Deck: Ryzen Z1 Extreme 8C/16T, RADV PHOENIX Mesa 26.2-devel, Proton 11.0, kernel 6.18 valve): Proton test lab, Linux profiling. Fully autonomous here, so launch, drive and close the game yourself.
- **WindowsAgentBedroom** (Windows PC): owns the code and builds. Sends `send-fixN` folders (totk.exe + totk.map) through the quintanar-chat hub.
- **Repo (private):** https://github.com/featureJosh/totk-pc-recomp-proton, branch `wine-dirty-tracking` (HEAD befd1c6 = fix14). No game files or ROM in it, ever: totk.exe/totk.map, romfs/exefs, caches, saves and logs are all derived from Josh's game. The Deck has no GitHub login.
- Rules: one hub message per result, no acknowledgements, @Josh only for decisions or a playable build. Josh asked for an hourly update when he's away.
- **Goal:** the Windows exe under Proton at 60 fps in the world, for playing on a Lenovo Legion Go S (lower power than this Deck). Windows performance no longer matters; Wine-only defaults may become unconditional.

## Current state (end of 2026-10-30)
- Installed: `totk.exe` = **fix14-O2** (befd1c6, sha 5d31d4a21675). Backups in `~/Games/TotK-Recomp/`: totk.exe.fix7 ... .fix13, .fix14o1, .fix14o2 (+ totk.map.fixN), .prev (original v143).
- `launch.env` = `TOTK_TRACE=perf` + `TOTK_IMPORT_CHUNK=0` (the latter is harmless in copy mode).
- Defaults under Wine now: TOTK_MEMORY=copy (no host-pointer imports), exact write watch with idle-region freezing, recorder thread on, own memcmp/memcpy, block pool for big allocations.
- Performance on Josh's Great Sky Island save: title, caves and menus at a locked 60. GSI standing 59.9 (at the cap, render busy ~11 ms). GSI walking 54-54.5 (render busy ~17 ms, but the render thread sleeps ~42% waiting for work).
- Saves: `profile/save` was restored to Josh's 14:33 state (backup `save-backup-2026-10-30-1433.tgz`). Test-run states: save-current-1820.tgz, save-after-tests-1906.tgz.
- Keep-awake: `systemd-inhibit` + `kde-inhibit` (both running `sleep infinity`) until reboot. Kill them when Josh is back if he wants normal sleep.
- `kernel.perf_event_paranoid=-1` is still set from last night (resets at reboot; to set it again, **ask Josh** for sudo).

## How to run a test
- Launch: `steam steam://rungameid/9747207482626801664`. Close: `pkill -x totk.exe`, then wait until `pgrep -x totk.exe` and `pgrep -x wineserver` are both empty.
- Log: `logs/totk.log` (rotates to .1-.4). Lines: `perf: frame N: interval ... render busy ... replay ... world X s`, `gpu avg ... bound:`, `mem: write-watch`, `mem: mirror`, `mem: traffic per frame`, `vk_usnap:`, `frametime: worst frame ... rest X`, `fps:`.
- **Lab scripts** in `~/Games/TotK-Recomp/labtools` (run with `S=~/Games/TotK-Recomp/labtools`):
  - `drive.sh '<cmds>'`: focuses the window; `tap KEY`, `hold KEY SECS`, `shot NAME` (spectacle screenshot into $S).
  - `load.sh`: title -> Continue -> top save -> Load, waits 15 s into the world.
  - `measure2.sh TAG`: standing 25 s (perf 12 s) + render-thread asleep % + game-thread faults/s + walking loop, with summary.
  - `abrun2.sh "fixA fixB ..." ROUNDS`: interleaved fresh launches (copies totk.exe.X into place, **restores the save folder from the 14:33 backup** so every run loads the same spot), runs load + measure2. ~10 min per run. Run it with nohup and wait on its PID.
  - Analysis: `summ.py` (averages by frame range), `split.py` (samples by origin via totk.map: game code vs runtime vs Wine/kernel), `ustk.py` / `exestk.py` (callers of frame-pointer-less code from `perf record --call-graph dwarf` + `perf report -D` raw stacks), `gfault.py` (game-thread fault/munmap shares).
- Keys (defaults): E = A, Tab = Plus, Escape = the recomp settings overlay, WASD = move, arrows = camera, M = Minus. **Take a screenshot before scripting keys**: presses that land in the settings overlay edit and save binds in totk.ini (that broke Josh's A/X once today).
- The game pauses when its window is unfocused. Run-to-run noise is about 3 fps, so only interleaved pairs count. Check every run's `*_pre.png`, because test autosaves can change which save loads.
- Don't use `pgrep -f`/`pkill -f` with a pattern that's in your own command line (it matched and killed my own shell today). Tracepoints (perf sched) need root; use `/proc/<pid>/task/<tid>/schedstat`, wchan and syscall instead.

## What we learned (short; details in the changelog)
1. GPU-imported host memory is a dead end on amdgpu (per-submit userptr revalidation). Proton hides VK_EXT_map_memory_placed. Copy mode is the answer.
2. Wine's CRT memcmp/memcpy are slow; we ship our own now.
3. Allocations above Wine's large-block threshold mmap/munmap every time, so pool them (fix12, fix14).
4. The render thread runs no game code; walking is limited by thread hand-offs (render waits ~7 ms per frame in thread_main on a futex), not by one CPU core.

## Open tasks
1. Walking at ~54 fps: find what the render thread waits for in thread_main (futex in libvulkan's .bss region, only the render thread waits on it) and who signals it late.
2. Coverage on fix14-O2 (-O2 game code risk): combat, gliding, fast travel, physics puzzles, a busy area (Lookout Landing), a 30+ min session.
3. Pipeline-build hitches (150-300 ms, rare): VK_EXT_shader_object is exposed by Proton 11 + RADV. WindowsAgentBedroom deferred it until frame cost is under budget.
4. Legion Go S: expect lower clocks; the recorder thread's spin should become a blocking wait.

## Misc
- Eden (Switch emulator) often runs in the background and uses ~1 core.
- dmesg is noisy with "VM memory stats ... non-zero when fini". Ignore it.
- `totk.ini.broken` = the bind-damaged ini from today, kept for reference.
