# Running under Proton (Steam Deck class hardware)

Goal: the Windows build of TotK Recomp runs well under Proton on Linux, not a native Linux port. The first test machine
was a Ryzen Z1 Extreme handheld (RDNA 3 iGPU, RADV, Mesa 26.2.0-devel, SteamOS kernel 6.18, Proton 11.0-100). This
file records what we measured, what the causes were, what changed in the runtime, and what is still open.

## Status (2026-10-29, branch `wine-dirty-tracking`)

| Build | Title | In world | Notes |
|---|---|---|---|
| upstream | 1-2 fps | - | every pool re-uploaded every frame |
| `TOTK_ASYNC_PREEMPT=0` only | ~2.3 fps | - | |
| hash tracker (49a8b76) | 8-19 fps | ~10 fps | reached the world |
| census skip (e91c92c) | 17-20 fps | - | |
| no write watch, THP (8e5429a) | 37-41 fps | 20-27 fps | intro and loading at a locked 60 |

Run with `TOTK_TRACE=perf` and nothing else: every Wine default below is chosen automatically (`wine_get_version` in
ntdll). `TOTK_IMPORT_CHUNK` stays 0 until the chunk fix below is done.

## Findings, in the order we found them

1. **Write watch over imported memory is wrong under Proton.** Proton 11 implements `GetWriteWatch` with userfaultfd
   write protection and `PAGEMAP_SCAN` ("kernel write watches"). For memory only the CPU touches it is exact
   (0.2-0.5 us/MiB). Any GPU access to a Vulkan host-pointer import, a read included, marks every page of the import as
   written: amdgpu takes userptr pages with write access, which clears the write protection. So every pool the GPU
   read came back fully written each frame. That caused re-uploads of ~84 MiB a frame, ASTC re-decodes, CPU
   re-deswizzles (`tex_bl_copy`) and ~200 ms of write-watch queries. Mirror mode does not escape it: the upload into
   the mirror is itself a GPU read of the import. Transparent huge pages are not the cause: one write reports one page
   either way. Diagnostic: `wwdiag.c` (reset/peek, commit/decommit, freeze/thaw, a Vulkan import, XXH3 speed).
2. **Async preemption is expensive under Wine.** It's SuspendThread/SetThreadContext, each a wineserver round trip
   plus a signal. Off: about a third faster per frame. The built-in sampling profiler (`TOTK_PROFILE`) crashes
   under Wine for the same reason; profile with Linux `perf` instead (`perf_event_paranoid=-1` for kernel stacks).
3. **The perf census distorted the measurement.** With `TOTK_TRACE=perf`, `vk_mem_dirty_submit` queried every
   buffer-bound pool at every submit, which in hash mode means hashing them: 800 of the 950 MiB hashed a frame.
4. **The real CPU cost was in the kernel, inside `vkQueueSubmit2`.** On every command submission amdgpu revalidates
   every userptr BO in the submission's BO list: `amdgpu_cs_ioctl` -> `amdgpu_ttm_tt_get_user_pages` ->
   `hmm_range_fault` -> `hmm_vma_walk_pmd`. That's ~1.06 GiB of imports, ~270k PTEs, twice a frame. It isn't
   Wine-specific (native Linux does the same), but Wine made it worse: a `MEM_WRITE_WATCH` reservation is registered
   for uffd-wp **and marked VM_NOHUGEPAGE** (smaps VmFlags `uw nh`), so the walk went one PTE at a time. Without the
   watch the heap gets THP (1780 of 1944 MiB huge) and `hmm_vma_walk_pmd` dropped from ~51% to ~5% of the replay
   thread.
5. **Next is the per-BO cost.** About 1570 userptr BOs (one per CPU-visible pool) are validated at every submission:
   `amdgpu_cs_ioctl` is ~60% of the replay thread in the world, mostly per-BO bookkeeping (`mtree_load`,
   `amdgpu_gtt_mgr_compatible`, `dma_resv_iter_next`, `__kvmalloc_node`).
6. **Chunked imports lose the device under Proton.** Binding pools into one import per aligned 128 MiB heap window
   (`TOTK_IMPORT_CHUNK`, 93e9b82) works on Windows. On the Deck the first submission failed with
   `VK_ERROR_DEVICE_LOST`, with no GPU fault in dmesg: the CS ioctl was rejected. The windows cover guest pages that are
   PROT_NONE (guard pages from SetMemoryPermission, `protect()`, uncommitted holes). amdgpu accepts the userptr at
   creation, but `hmm_range_fault` returns -EFAULT on those pages at submit time. Windows never checks.

## What changed in the runtime

| Commit | Change | Setting |
|---|---|---|
| d1f016b | async preemption defaults to off under Wine (Legolas's patch, kernel.c `ap_init`) | `TOTK_ASYNC_PREEMPT` |
| 49a8b76 | hash dirty tracking: an XXH3 hash per page of every import, compared at query time; imported pages are never reset (memory.c `written_hashed`, `mem_dirty_track`, `mem_dirty_absorb`) | `TOTK_DIRTY=auto\|writewatch\|hash` |
| 152016c | large hash queries on thread-pool helpers | |
| e91c92c | the perf census skips itself in hash mode | |
| 45a3b07 | helper count setting | `TOTK_DIRTY_HELPERS=auto\|0..8` (auto = 0 under Wine) |
| 8e5429a | under Wine the heap and alias reservations have no write watch: THP back the heap, the hash tracker covers every import, other pages read as unwritten | `TOTK_WRITE_WATCH=auto\|on\|off` (environment only) |
| 93e9b82 + follow-up | import chunks, off by default after finding 6 | `TOTK_IMPORT_CHUNK=0\|2..4096` |

Windows behaviour is unchanged by default: writewatch, preemption on, write watch on, no chunks.

## Next steps

1. **Heap as GPU memory (the real fix).** SavageWindRecomp hit finding 4 on the Steam Deck: "amdgpu revisits every
   page of an imported host-memory buffer at each submission: ~4 ms a submit for the 3 GB guest heap". Their fix is to
   make the guest heap GPU memory, exported as a dma-buf and `mmap`ed over the heap's addresses
   (`runtime/nvn/vulkan/exec.c`, `backend_heap_set`). A Windows exe under Wine can't mmap a dma-buf. The equivalent
   would be `VK_EXT_map_memory_placed`: allocate the heap as host-visible (cached) device memory and map it AT the heap
   address, so pools become buffers bound at offsets with no userptr at all. That removes the per-submit validation,
   and probably the hashing too, if the write watch works on that mapping. To check: does Proton's winevulkan expose
   `VK_EXT_map_memory_placed` to the app, and how does it interact with `commit`/`decommit`/`protect` in memory.c?
2. **Safe chunks (fallback).** Form a chunk only over a window that's wholly committed read-write (VirtualQuery). Make
   `protect()`/`decommit()` drop or split a live chunk before a guest page in it becomes PROT_NONE: the guest can change
   permissions later.
3. **Copy mode (fallback).** No imports under Wine at all: ordinary host-visible buffers, an exact write watch for CPU
   uploads, and GPU-written ranges copied back to guest memory at fence completion.
4. Remaining hash cost: ~11 ms a frame in the world (230 MiB). It goes away with 1 or 3.

## Reference projects (kept in `C:\Users\jshqn\Projects\ref`)

- **SavageWindRecomp** (the most useful): same code family, native Linux and Steam Deck. `docs/linux.md` lists the
  Deck fixes: `VK_KHR_maintenance5` for point lists without a point size (a GPU hang), the
  `ATTACHMENT_FEEDBACK_LOOP_OPTIMAL` layout on RADV (no `VK_KHR_unified_image_layouts`), and the heap as dma-buf GPU
  memory.
- **LASRecomp**: the ancestor; on Linux 6.7+ its write tracking uses userfaultfd + PAGEMAP_SCAN, like Proton's.
- **UltraTears** (TotK 1.4.3, Windows): imports guest memory with `VK_EXT_external_memory_host` like us; its Linux
  support is only planned so far.
- **SkyboundTotk** (TotK 1.0.0, Windows, WebGPU/wgpu renderer): a different renderer, nothing Proton-specific.

## How to test

- Deck: `~/Games/TotK-Recomp`, Steam launch options `~/Games/TotK-Recomp/run.sh %command%`, settings in `launch.env`.
  Send only `totk.exe` and its `totk.map` (the DLLs, romfs and caches don't change).
- Windows check before each send: `ninja -f build/v143/build.ninja` from the repo root (MSYS2 UCRT64 on PATH),
  `totk.exe --selftest`, then a headless run from `build\v143`:
  `TOTK_ASSUME_FOCUS=1 TOTK_MEMORY=import TOTK_INPUT=none TOTK_TRACE=perf timeout 70 ./totk.exe --seconds 60`. To
  mimic the Wine path, add `TOTK_DIRTY=hash TOTK_WRITE_WATCH=off`.
- Log lines to compare: `frametime`, `perf: slow frame`, `mem: dirty hash`, `mem: write-watch`, `perf: frame N:
  texture re-uploads`, `memory: dirty tracking ...`, `kernel: asynchronous preemption ...`.
