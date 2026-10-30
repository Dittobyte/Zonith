# TotK Recomp on Linux/Proton: day log, 2026-10-30 (Deck lab, Legolas)

Hardware: Ryzen Z1 Extreme (8 cores / 16 threads, ran at ~4.6 GHz on AC), RADV PHOENIX (Mesa 26.2-devel), Proton 11.0, 1920x1080 output (scene 1600x900, FSR). Windows exe under Proton, no native port.
Test spot: Josh's Great Sky Island save (manual save, 14:30). Each build gets fresh launches, interleaved with the previous build (A B A B), 25 s standing (with a perf capture) and then a ~45 s walking loop. Run-to-run noise is about 3 fps, so only paired results count.

## Where we started and ended

| Scene | Start of day (fix5/fix6) | End of day (fix14-O2) |
|---|---|---|
| Title screen | 37-41 fps | 60 (locked) |
| Prologue cave / menus | 20-27 fps (original exe: ~2) | 60 (locked) |
| Great Sky Island, standing | not reachable (no save) / ~45 on fix7 | 59.9 (at the 60 cap) |
| Great Sky Island, walking | ~45 on fix7 | 54-54.5 |

## Build by build

| Build | Change (WindowsAgentBedroom) | Deck result (GSI, standing / walking) | Verdict |
|---|---|---|---|
| (morning check) | Can the heap become GPU memory via VK_EXT_map_memory_placed (SavageWind's Linux fix)? | Proton 11 / Experimental hide the extension (RADV has it). Not possible on stock Proton | dead end, copy mode instead |
| fix7 | Copy mode: no host-pointer imports; GPU gets mirrors, CPU copies written pages | caves 60; GSI 45.6 / 45.7. amdgpu_cs_ioctl on the submit thread 60% -> 3% | huge win (30 -> 60 in caves) |
| fix8 | Exact write watch + freezing idle regions in copy mode | 45.6 -> 55.2 / 45.7 -> 48.8. write-watch 1.9 -> 0.5 ms, kernel time on the render thread 20% -> 9% | big win |
| fix9 | Own AVX2 memcmp (Wine's is scalar) | +1.3 to +1.6 fps standing, walking neutral | small win |
| fix10 | Own memcpy (AVX2 / rep movsb) | memcpy 16% -> 10% of the render thread, fps neutral | neutral |
| fix11 | Trimmed per-draw constant-bank compare/copy | neutral | neutral |
| fix12 | Reuse big recording blocks instead of malloc/free each frame | 53.5/57.1 -> 58.7/59.1 standing, 50 -> 53.5 walking; render busy under 16.7 ms for the first time | big win |
| fix13 | Descriptor-set updates moved to the recorder thread; batched set allocation | standing at the 60 cap (busy ~11-13 ms), walking ~54 | win |
| fix14-O1 | Command-buffer recording buffer from the block pool (no realloc) | game-thread page faults 110k/s -> 63k/s; fps same as fix13 | win (efficiency) |
| fix14-O2 | Same + game code compiled at -O2 | faults 49k/s; standing 59.9, busy 10.5-11.3 ms; walking 54.0-54.5 | best build; installed |

## What each finding was

1. GPU-imported memory under amdgpu (yesterday's root cause) is gone entirely with copy mode: no userptr BOs, no per-submit page-table walk, no hashing.
2. Wine's CRT is slow for this workload: memcmp is scalar (8 bytes per step) and memcpy plain SSE2. Our own versions removed ~11% and ~6% of render-thread time.
3. Allocation churn: blocks over Wine's large-block threshold go straight to mmap/munmap, so every frame re-faulted and re-zeroed fresh pages. Pooling them (fix12, fix14) was the biggest late-day win.
4. The render thread now runs zero game code and sleeps ~42% of the time waiting for work. Walking is limited by hand-offs between threads, not by one saturated CPU core (the busiest thread is ~55-57% on-CPU).

## Pros and cons / risks of today's changes

| Change | Pro | Con / risk |
|---|---|---|
| Copy mode (mirrors) | Removes the amdgpu userptr cost; 60 fps in caves | Extra copies each frame (~0.6-0.8 MiB up, ~0.2 MiB back); a sync bug would show as stale or garbled geometry (none seen) |
| Write-watch freezing | Tracking cost down 4x | Frozen regions fault once when written ("thaw"); more state that can go wrong |
| Own memcmp / memcpy | Faster than Wine's CRT | Our memcpy doesn't handle overlapping copies (C doesn't require it, but a hidden overlap would corrupt silently; none seen) |
| Block pool (fix12/fix14) | Kills per-frame mmap/munmap and page zeroing | Keeps up to 256 MiB of freed blocks resident (more RAM in use) |
| Descriptor updates on the recorder thread (fix13) | Less work on the render thread | Ordering bug risk: a missing or flickering texture would be the symptom (none seen) |
| -O2 game code (fix14-O2) | Faster game threads (fewer faults, more headroom) | -O2 can expose undefined behaviour in the generated C as game-logic bugs. Only walking/camera was tested: no combat, gliding, physics puzzles or cooking yet |
| Recorder thread | +1.7 ms/frame for the render thread | Spins a core while waiting: costs power/clocks on the Legion Go S's smaller budget |

## Still open

- Walking is ~54 fps: the render thread waits ~7 ms a frame in its main loop. Next: find what it waits for and who signals it late.
- Rare 150-300 ms hitches from pipeline builds (seen in Josh's first session). Candidate fix: VK_EXT_shader_object (Proton 11 + RADV expose it).
- Coverage: only Great Sky Island and the prologue cave. Need: combat, gliding, fast travel, a busy area (Lookout Landing), and long sessions.
- Legion Go S (Z2 Go, lower power): expect lower numbers than the Deck's 4.6 GHz Z1 Extreme; the recorder spin should become a blocking wait there.

## Lab notes

- Tools: ~/Games/TotK-Recomp/labtools (keyboard + screenshot driver, save loader, measure/abrun A/B scripts, perf helpers for frame-pointer-less code). Each run restores the save folder from save-backup-2026-10-30-1433.tgz, because test autosaves move the load list.
- Mistake today: scripted key presses landed in the settings overlay and cleared Josh's controller bindings (A/X); fixed by deleting the bind lines from totk.ini. Screenshot before scripting keys.
