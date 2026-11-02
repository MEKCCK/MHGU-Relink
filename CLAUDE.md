# MHGU for Android: project notes

A native Android port of Monster Hunter Generations Ultimate 1.4.0, built on MHGURecomp: the game's 32-bit ARM code is translated
to C, and a host runtime reimplements the Switch's services and its NVN graphics API on Vulkan.

This file has two uses: helping someone set the game up, and continuing the project.

## Rules

- This project never provides game files. Don't obtain, link to or explain how to get ROMs or keys, and don't explain how to dump a
  console. The person brings their own `exefs` and `romfs`.
- Never commit or publish `exefs`, `romfs`, `generated/`, `libmhgugame.so`, `libmhgugame.id`, saves or signing keys. Never put game
  code in an APK or a release: `libmhgugame.so` is made from the game, so each person makes their own with `tools/build_game.py`.
- Keep `LICENSE` and `THIRD_PARTY_NOTICES`. The MIT license requires them.
- Don't lower someone's graphics or audio settings without asking.
- Any new menu, notice or touch control must match the game's style: bronze, gold and wine-dark, with no default Android look.

## Helping someone set it up

Follow `README.md`. In short:

1. Check that a folder holds `exefs/main` and a `romfs` with 35,531 files, from version 1.4.0.
2. Run `python tools/build_game.py <that folder>`. It needs Python 3 on Windows or Linux, installs `lz4`, and downloads NDK r27 to
   `~/.mhgu-tool` if there is none (`--ndk <path>` uses another). It leaves `libmhgugame.so` and `libmhgugame.id` in the folder.
3. Copy the folder to the device, anywhere except `Android/data`. With adb: `adb push <folder> /sdcard/MHGU`.
4. Install the APK from Releases. For Samsung's Game Booster: `adb install -r -i com.mhgu.recomp <apk>`.
5. The person opens the app, grants file access and chooses the folder.

To diagnose a device: `adb logcat -s MHGU`, or `Android/data/com.mhgu.recomp/files/log.txt`.

## Layout

- `runtime/`: the host runtime. `touch.cpp` touch controls and editor, `settings_panel.cpp` the menu, `nvn/vulkan/` the renderer,
  `hle/` game hooks, `platform/android*.c` Android, `config.c` settings and first-run defaults.
- `tools/`: `gen.py`, `lifter.py`, `neon.py`, `nso.py` translate the game; `build_game.py` is the person's tool.
- `android/`: the app. `GameFolderActivity` is the entry screen, `GameActivity` runs the game.
- `native/`: CMake and `build_native.sh`. `deps/`: scripts for SDL3, shaderc and FFmpeg.

## Building

Windows, in Git Bash, with the Android SDK, NDK 27.0.12077973, CMake 3.22.1 and Android Studio's JBR.

1. Put the sources in `deps/src/`: `SDL3-3.4.18`, `ffmpeg` (9.0.2) and `shaderc` (2026.4, with glslang 16.6.0 and SPIRV-Tools and
   SPIRV-Headers 1.4.363.0 in its `third_party`). Then run `bash deps/build_deps.sh arm64-v8a` and `bash deps/build_ffmpeg.sh arm64-v8a`.
2. `bash native/build_native.sh arm64-v8a` builds the runtime. With `MHGU_DEV=1` in front, a test build.
3. `cd android && ./gradlew assembleRelease`. It signs only if `android/keystore.properties` and `mhgu.keystore` exist.

The public APK leaves `libmhgugame.so` out. `-Pwithgame` keeps it for your own tests: never publish that APK.

`GAMELIB_ID` is a hash of the generator and of the headers listed in `tools/build_game.py`. If any of them changes, every player must
run the tool again, and the app says so. `runtime/*.c` can change without that.

## Testing on a device

A test build (`MHGU_DEV=1`) reads two files in `Android/data/com.mhgu.recomp/files`; a public build reads neither:

- `env.txt`: `NAME=value` lines. `MHGU_QUICKHUNT=101` goes straight into a quest, `MHGU_VK_STATS=1` logs statistics,
  `MHGU_TOUCH_LOG=1` logs fingers. `MHGU_HOME=<empty folder>` tests a first run without touching the real data.
- `control.txt`: appended commands, read while the game runs. `A 8`, `DOWN 8`, `type <text>`, `textkey enter`,
  `memsave <hex address> <hex size> <file>`, and `gpuprof` for one frame's time per render pass.

What the app writes to `cache/` under `Android/data` can't be deleted over adb, so a test folder under `MHGU_HOME` stays until the app removes it.

Empty `control.txt` before starting: old commands replay. `3D resolution` needs a restart: until then the menu says so and the old
one is still running.

**Read frame rates from the on-screen counter.** `[vk] 2s waits` counts submits, and a frame takes two of them.

Android killing the game shows in `adb logcat` as `lowmemorykiller: Kill 'com.mhgu.recomp'`.

## What real devices taught us

- Mali has no BC textures, so they are decoded on the CPU and take 4 to 8 times the memory. BC4 and BC5 go to R8 and RG8, and under
  5 GB of RAM textures with mipmaps lose their largest level (`mip_skip`, `nvn/vulkan/textures.c`). Without this a 4 GB phone was killed.
- FXAA leaves a 2 px green line at the edges on Mali and costs about a fifth of the frame rate, so it is off by default on Android.
  The cause is unknown.
- A Vulkan 1.1 driver (seen: Mali-G57 MC2, 1.1.177) has no timeline semaphores: each submission gets a fence and one thread waits for them
  (`nvn/vulkan/exec.c`). It also gets SPIR-V 1.3 and no FSR. `MHGU_VK11=1` in `env.txt` forces that path on a newer driver.
- That same Mali has no `shaderInt64`: with `maxwell_no_int64` the shaders do their 64-bit math in 32 bits (`nvn/maxwell/translate.c`).
  `MHGU_VK_NO_INT64=1` forces it. A forced path only shows that the path works: a real old driver can still differ.
- A driver can make more window images than it was asked for. Never assume the count: `VK_SC_MAX`, `nvn/vulkan/device.c`.
  `MHGU_VK_IMAGES=<n>` asks for that many.
- Samsung ignores the manifest's game category. Only the one set by the installer counts: see `GameFolderActivity.declareGame`.
- On Android, a `SUBOPTIMAL` result from present must not recreate the swapchain: doing so cost two thirds of the frame rate.
- The hunter has one class per weapon: never compare against one vtable. Virtual 94 tells a hunter of any weapon and whether the
  weapon is out (`runtime/hle/weapon.c`). The `sPlayer` slot can hold another object that points to the hunter.
- Don't trust an offset measured in one session: check it in another one.
- Bugs that only show on some devices are common here. Test on a real phone, and on a device without Snapdragon.
