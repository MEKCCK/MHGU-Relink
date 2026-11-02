# MHGU for Android

Play your own copy of Monster Hunter Generations Ultimate on Android, natively and with touch controls made for phones.
It is not an emulator: the game's code is translated to run directly on your device.

**This project includes no game code, game files, ROMs or keys. You need your own copy of the game.**

Made with Claude Code (Opus 5.5). Many of the decisions were my own, because AI isn't ready to have the final say on something like this.

[Lee esta guía en español](README.es.md)

## What it does

### Touch controls for the hunt

- **Movement:** a fixed pad that doesn't follow your finger. Drag past its edge to run; this turns off while your weapon is out, so you don't run off in the middle of a fight
- **Camera:** drag anywhere on the screen to turn it, as in a phone game. A double tap centers it
- **Monster lock-on:** tap the target icon and the camera follows the monster until you tap it again, as in the newer Monster Hunter games. It only follows a monster in your zone. You can turn it off under the gear, **Controls**, **Lock-on**: the icon then turns the camera once, as in the original game
- **A layout of their own for bows and bowguns:** with one in hand, in a hunt, the controls change by themselves: a large fire button under your thumb, reload, the scope with a tap or quick aim if you hold it, crouching fire, and arrows for the ammo. Drag on the fire button while you hold it to aim. It follows the game's Bow Controls option and has its own place in the editor
- **Change ammo with a swipe:** besides the arrows, swipe up or down on the item bar. The game's list now shows whole, with each ammo's name
- **Shots with a monster locked on:** they go toward it, even if it is higher or lower than you. To raise the shot, a bowgun goes into quick aim for a moment. Without the lock you aim as always
- **Items:** the bar sits at the bottom center. Tap it to use the item and swipe it to change the item
- **Hunter Arts:** tap each one's panel to use it
- **Attacks:** three layouts to choose from: a diagonally split pill, separate buttons, or a U-shaped band in the corner of the screen
- **Other buttons:** dodge, guard and sheathe, each with its own icon

### Menus and comfort

- **Menus made for your thumb:** the movement stick stays where it is and moves the cursor. Confirm and Back sit on a pill in the same place as the attacks, and dragging on the screen moves the cursor too. L and R are tabs
- Your phone's keyboard works for typing your character's name
- The settings menu works by touch and matches the game's look
- **Faster drinking and eating:** two optional cheats, off by default, under the gear, **Cheats**. They speed up the animation x1.7 or x2.5, but they don't let you walk while drinking
- Import your save from an emulator or another device, and export it to take it to an emulator
- The game pauses by itself when you leave the app
- Vibration on button taps, and the game's rumble on your phone, with or without a controller

### Customization

- Press and hold the gear to open the editor: move and resize each button, and move the item bar, the Hunter Arts and the target icon
- The game's HUD is rearranged to stay clear of your thumbs. You can turn this off
- Adjustable control opacity and camera speed
- **Buttons on the screen:** under the gear, **Controls**, **Buttons on the screen**, choose which ones show. You can add L, ZL, ZR, -, the d-pad and the stick clicks, which are hidden by default. In town ZL always shows, because it opens the multiplayer menu
- **Combined buttons:** on that same page, make up to 6 buttons of your own that press several at once, such as A+X. Place them with the editor, and you can also put them on a controller button

### Display and performance

- Up to 120 fps on powerful phones, with a configurable frame rate limit
- Fills the whole screen, from ultra-wide phones to 4:3 handhelds
- Adjustable 3D resolution to gain performance on modest devices
- **Low-end mode:** for weak phones. It turns off bloom and depth of field, uses lighter textures and keeps the HUD sharp over a smaller 3D picture
- Optional FPS and temperature counter
- Uses less memory on devices with Mali GPUs and on phones with little RAM
- The screen follows your FPS limit: at 60 fps it runs at 60 Hz even if it can do 120
- **If your phone runs hot, lower the FPS limit.** Nothing helps more: going from 120 to 60 used a quarter to a third less power on a Galaxy S25 Ultra, and going from 60 to 30 a fifth less on a Moto g06
- On Mali GPUs textures are decoded on several cores, to reduce stutter
- **Your own GPU driver:** Adreno only, at your own risk. Under the gear, **Graphics and audio**, **GPU driver**, add a .zip with `meta.json` and the library. By default the system's is used, and if one fails at start the app goes back to it by itself

### With a controller

- The touch controls hide as soon as you connect one and come back when you disconnect it, with no restart
- Press **+** and **-** together to open the settings
- Clicking the right stick locks onto the monster
- **Any controller:** if the game doesn't read yours well, go to the gear, **Controls**, **Map a controller**, and press each button once. It's kept for that model

### Local play

Two devices on the same Wi-Fi can play in the same hub. In town, tap ZL and choose **Connect**. One goes to **Create Hub** and confirms with X; the other goes to **Search Hubs** and picks the hub. If it doesn't show up, enter the host's IP under the gear, **Network**, **Search hosts**. Creating a hub, finding it and joining it were tested on a Motorola and an Anbernic, but not a full quest yet.

### Inherited from the PC version

Mods, adjustable monster HP and Village quests in the Hub are also in the settings, but they haven't been fully tested on Android yet.

## How well does it run?

Measured with the on-screen counter. In the Low-end mode column the 3D picture is at 40% and the FPS limit was raised, except for the 5-minute session, which used the mode's 30 fps limit:

| Device | Chip | GPU | RAM | Normal | Low-end mode |
|---|---|---|---|---|---|
| Galaxy S25 Ultra | Snapdragon 8 Elite | Adreno 830 | 12 GB | 120 fps | 120 fps in the village |
| Anbernic RG406V | Unisoc T820 | Mali-G57 | 8 GB | 48 fps at full resolution, 60 fps at 50% | 60 fps |
| moto g06 power | MediaTek MT6769 | Mali-G52 MC2 | 4 GB | 16 fps at full resolution, up to 28 fps at 50% | 45 fps; a steady 30 fps over a 5-minute session |

On a phone with 4 GB of RAM, close your other apps first: Android can close the game when memory runs out. For more speed,
turn on Low-end mode: go to the gear button, **Graphics & Audio**, **Low-end mode**, save, and restart the game. It sets the 3D
resolution to 40% and the limit to 30 fps, and you can change both. It costs some sharpness: the game looks softer.

## What you need

- **An Android device:** Android 10 or newer, 64-bit, with Vulkan 1.2. Almost any device from the last few years. Vulkan 1.1 works experimentally. On processors from before 2018 the app adapts the game code the first time it starts, which takes a few seconds.
- **About 13 GB free** on the device.
- **A Windows or Linux PC,** only for setup, with about 10 GB free.
- **Your own copy of the game,** updated to version 1.4.0.

## Setup

It takes three steps, and you only do them once.

### Step 1: Get your game files

You need two folders from your own game: `exefs` and `romfs`. They must include the 1.4.0 update.

If your copy is in the Ryujinx emulator on your PC:

1. Make sure the 1.4.0 update is applied to the game.
2. Right-click the game, open **Extract Data**, and extract **ExeFS** and then **RomFS** into one empty folder.

Keep both in one folder, for example `C:\MHGU`:

```
MHGU
├─ exefs
└─ romfs
```

This project does not explain how to dump a game from your console, and does not provide game files, keys or links to them.

### Step 2: Create the game code

The app has no game code in it, so you create yours from your own copy. It runs on your PC and sends nothing anywhere.

1. Install [Python](https://www.python.org/downloads/). On Windows, tick **Add python.exe to PATH** in the installer.
2. On this page, click the green **Code** button, then **Download ZIP**, and unzip it.
3. Open the unzipped folder, then its `tools` folder.
4. Windows: click the address bar of that window, type `cmd` and press Enter. Linux: open a terminal in that folder.
5. Type this, with your own folder, and press Enter:

   ```
   python build_game.py C:\MHGU
   ```

The first time, it downloads Google's Android compiler (about 1.8 GB). Then it works on its own: about 6 minutes on a fast PC,
longer on a slower one. When it says **Done**, your folder has two new files:

```
MHGU
├─ exefs
├─ romfs
├─ libmhgugame.so
└─ libmhgugame.id
```

**These two files are made from your game: keep them for yourself and don't share them.**

### Step 3: Install it

1. Copy the whole `MHGU` folder to your device, for example with a USB cable. It can go anywhere in its storage or on an SD card,
   except inside `Android/data`.
2. On your device, download the APK from [Releases](../../releases/latest) and open it. Android may ask you to allow installing
   apps from your browser.
3. Open **MHGU**, tap **Give permission and choose folder**, and turn on file access when Android asks. The game only reads that folder.
4. Find your `MHGU` folder and tap **Use this folder**.

The app checks the folder, tells you exactly what is missing if something is wrong, and starts the game.

Your saves and settings are kept separately, in `Android/data/com.mhgu.recomp/files`. To choose another folder later: gear
button, **Menu Options**, **Game folder**.

## Import your save

You can bring your save from an emulator or another device:

1. Find your save's file named `system`. In Ryujinx: right-click the game, **Open User Save Directory**. In yuzu: right-click the game,
   **Open Save Data Location**.
2. Copy that file to your device, for example to Downloads.
3. In the game, go to the gear, **Menu Options**, **Import save**, and choose the file.

All the characters on the device are replaced and the game restarts. Your previous save is kept in
`Android/data/com.mhgu.recomp/files/MHGURecomp/save_backups`.

## Export your save

To take your save to an emulator or another device:

1. Save in the game. Then go to the gear, **Menu Options**, **Export save**, and choose a folder.
2. A folder named `MHGU_save_...` is made there with the files `system` and `system_backup`. Move it to your PC.
3. In Ryujinx: right-click the game, **Open User Save Directory**. In yuzu: right-click the game, **Open Save Data Location**.
4. Keep a copy of what is there, and copy both files in.

## Samsung: Game Booster

Samsung's performance panel only appears for apps it recognizes as games, and an APK installed by hand doesn't count. To get
it, install the APK once from a PC with [adb](https://developer.android.com/tools/adb):

```
adb install -r -i com.mhgu.recomp MHGU-0.1.5.apk
```

Use the name of the APK you downloaded. This is optional: the game runs the same without it. To at least see it in Gaming Hub: **My games**, **Library**, the three
dots, **Add games**.

## If something goes wrong

| Problem | What to do |
|---|---|
| `python` is not recognized | Install Python again, tick **Add python.exe to PATH**, and open a new terminal |
| "there is no exefs/main" | The path must be the folder that holds `exefs` and `romfs` |
| "This exefs is not version 1.4.0" | Use the `exefs` with the 1.4.0 update |
| "romfs is incomplete" | Use the base game's `romfs` with the update applied. It has 35,531 files |
| "libmhgugame.so is missing" | Do step 2, then copy the two new files to the folder on your device |
| "This libmhgugame.so is from another version" | A new app version needs it again: repeat step 2 with the new download |
| The game closes by itself | Close other apps and lower the 3D resolution. It's usually a lack of memory |

If it still fails, open an [issue](../../issues) with your device model and the file
`Android/data/com.mhgu.recomp/files/log.txt`. Don't attach game files.

## Legal

This project is not affiliated with, endorsed by or sponsored by the Great C or the Great N. Monster Hunter is a trademark of the Great C.
Switch is a trademark of the Great N.

This repository and its releases contain no game code, game data, ROMs or keys. Use it only with a copy of the game you own.
Requests for game files, keys or ready-made `libmhgugame.so` files will be closed.

Rights holders: please open an issue to get in touch.

## Credits

Based on the open-source MHGURecomp project, under the [MIT license](LICENSE), and the libraries listed in
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES). Their licenses are also in the app: gear button, **Menu Options**, **Licenses**.

## For developers

See [CLAUDE.md](CLAUDE.md) for how the project is organized, built and tested, and for step-by-step setup notes.
