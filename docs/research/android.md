# TODO: Android build (planned 2026-10-25, assessed 2026-10-31, started 2026-10-31: BC textures)

Goal: the port on Android phones / tablets, played with a controller (Bluetooth or USB). No touch controls: the game
doesn't suit them, so none are planned. Players bring their own dump as on PC; the APK holds no game files.

Verdict (2026-10-31): viable. The CPU / OS side is small and well defined; the open-ended part is the GPU side
(phone drivers, tile-based GPUs). Four things below stop it booting or drawing correctly ("Must fix").

## What carries over

- The recompiled game code and the runtime are plain C (the lifter's output, interpreter, NEON emulation): no x86
  intrinsics anywhere. Only the build flag `GENARCH ?= -march=x86-64-v3` (Makefile) is x86; arm64 needs its own
  (e.g. `-march=armv8.2-a`). The guest's NEON / VFP could even map to the host's NEON later, for speed.
- The guest's memory ordering carries over: the lifter turns DMB / DSB into `__atomic_thread_fence` and LDREX / STREX
  into seq_cst helpers, so a weakly ordered arm64 host is safe (x86's TSO never tested that, but the guest itself was
  ARM). Float behaviour (NaN, conversions) is if anything closer to the guest's on arm64 than on x86.
- No JIT: nothing needs executable memory at run time.
- runtime/platform/linux.c is the starting point: Android is Linux with bionic (mmap, signals, threads).
- SDL3 supports Android (its Java activity glue, gamepads, audio, text input with the system keyboard for names and
  item set names). Controller-only fits SDL's gamepad API as it is.
- Vulkan is Android's graphics API, and mobile GPUs decode ASTC natively (the port's ASTC decoder can be skipped
  there).
- The settings menu (F1 / Plus + Minus) already works with a controller: no ini editing needed on a phone.

## Must fix (it doesn't start, or draws wrong, without these)

1. GUEST_BASE (runtime/cpu.h) is 1 TB. Most Android kernels have a 39-bit address space: user space ends at 512 GB,
   so plat_reserve_guest fails at startup. Pick a lower constant (e.g. 64 GB, 0x1000000000) the platform leaves
   free. G2H is a macro: recompiling the generated code is enough, no new `make gen`.
2. BC textures: Adreno and Mali drivers on Android have no textureCompressionBC. textures.c decoded only ASTC on the
   CPU when the GPU lacks a format; anything else got an empty RGBA8 image (black). DONE for BC1-BC5: see "BC
   textures" below.
3. Shader int64: every translated shader has `#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require`
   (maxwell/translate.c), while device.c only logs a warning without shaderInt64. Not every phone driver has it: there
   every pipeline would fail. Check the target GPUs (gpuinfo.org), or make the translator do without int64.
4. The fault handler (linux.c, the SIGSEGV handler that backs guest pages on first touch) reads x86 registers
   (REG_ERR, REG_RIP, REG_RSP). arm64: uc_mcontext.pc / sp / regs[30], and the write bit from the ESR record in
   __reserved. Android's runtime chains its own signal handlers in front of the app's (libsigchain): emulators live
   with it, but test it.

Smaller, for bionic: runtime/platform/linux_glibc.c (glibc symbol versions) doesn't apply; execinfo's backtrace()
needs API 33 (or use the unwinder); posix_spawn needs API 28.

## Work

1. Build: NDK clang for arm64-v8a, the generated code as a shared library (mhgugame.so, as on Linux; ~90 MB like the
   Windows DLL, so build time and APK size need watching), a Gradle project around SDL3's Android template, an APK.
   FFmpeg, shaderc and SDL3 cross-built for arm64 as the Makefile already builds them for Linux.
2. Game files (12.3 GB): Android's scoped storage. Simplest for a sideloaded APK: the "All files access" permission
   (MANAGE_EXTERNAL_STORAGE): the player copies exefs / romfs to e.g. /sdcard/MHGURecomp/data and the runtime keeps
   opening paths as now. (The Play Store would refuse that permission; we don't use it.) The alternatives: copy them
   into the app's own storage (doubles the space), or Storage Access Framework file descriptors (rework of romfs.c and
   the fs service). Saves, ini, cache, mods and cheats beside them.
3. Vulkan on mobile drivers (Adreno, Mali, Xclipse): "Must fix" 2 and 3; shaderc on Android (it compiles the game's
   shaders at run time: slower on a phone, the prewarm takes longer); memory (phones share RAM between CPU and GPU).
   Adreno's stock drivers are known for bugs (emulators offer Mesa's Turnip instead).
4. Movies: ffmpeg's avformat / avcodec built for Android, or MediaCodec.
5. 16 KB pages: PLAT_PAGE is fixed at 4 KB. Some newer devices (Android 15+) run 16 KB page kernels: plat_protect /
   commit granularity. Fine for a first version on 4 KB devices.
6. Lifecycle: the app paused / resumed (audio, the Vulkan surface going away: swapchain recreated), Android may kill
   a backgrounded app, the back button.
7. LAN multiplayer: broadcast discovery needs Android's WifiManager.MulticastLock (one JNI call). The hub server works
   as it is.
8. Left out on Android: Discord Rich Presence, the updater (an APK updates otherwise).

## Performance

- CPU: the Switch runs the game on Cortex-A57 cores at ~1 GHz. Ahead-of-time compiled C on a recent 2.4-3 GHz phone
  core has a lot of headroom: expected 30 fps comfortable on 2021+ mid / high range, 60 plausible on flagships.
  Not measured.
- GPU, the real risk: every render pass loads and stores its attachments (exec.c: LOAD_OP_LOAD / STORE_OP_STORE).
  Cheap on desktop GPUs; on tile-based phone GPUs each pass reads and writes every attachment through memory. Levers:
  RenderResolution, the FPS cap, CLEAR / DONT_CARE where the game clears anyway.
- Thermal throttling over a long hunt.

## BC textures (started 2026-10-31)

Prototyped on PC first, where it can be tested with the game as it is: `MHGU_NO_BC=1` makes the renderer act as if
the GPU had no textureCompressionBC (what an Android phone reports), so BC textures take the CPU decode path. The
game uses BC1, BC3, BC4 and BC5 (no BC6H / BC7 / ASTC seen); runtime/nvn/bcn.c decodes them, and the hunt and the
village look the same as with the GPU's decode. Details: docs/STATUS.md "BC textures without GPU support". Left for a
phone: the decode costs CPU time at loads (not measured on a phone), and the decoded images take 2x (BC4 / BC5) to
8x (BC1) the memory of the compressed ones. If that hurts: transcode to ETC2 (which every phone GPU samples), or
decode in a compute shader.

## Open questions

- Minimum device: arm64 only; Android 10 / 11+, Vulkan 1.2 with timeline semaphores and shaderInt64; roughly
  Snapdragon 865 / Adreno 650 class or better, 6 GB RAM.
- Distribution: an APK on the releases page (sideloaded), like the PC zips.
- Multiplayer works the same over Wi-Fi (Local Play over IP; the internet hubs plan in internet-hubs.md).
