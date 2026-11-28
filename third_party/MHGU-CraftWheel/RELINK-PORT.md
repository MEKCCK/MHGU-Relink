# MHGU-CraftWheel in MHGU-Relink

Source of the shortcut wheel, fetched from <https://github.com/NovemLaurant/MHGU-CraftWheel> at commit
`18164d823a0519be79ce4429514c1753ac20dedf` (2026-11-20), release v0.51.

Relink's built-in wheel (`runtime/touch.cpp`: `HUD_WHEEL` / `HUD_BAR_WHEEL`) follows the wheel in MHGU-Xth's port;
this tree is the upstream reference and remains usable as a Forge plugin: Relink loads Forge plugins through
`runtime/forge.c`.

```sh
python3 build_mod.py port --output out/pc/gu_craft_wheel.nro
```

The built `GU-CraftWheel` folder goes in the game's `mods/` directory (see `docs/MODS-zh-CN.md` in the Relink tree).
Do not run the Forge wheel and the built-in wheel in the same quest: both would react to the same buttons. The
six-slot item bar (`runtime/hle/hudbar.c`, `runtime/hle/custom_itembar.c`, drawn by `runtime/touch.cpp`) is part of
the built-in `Bar` / `BarWheel` HUD.

The author granted MIT for this vendored copy (Copyright (c) 2026 NovemLaurant); see `LICENSE`. Upstream ships
licenses for its third-party dependencies in `licenses/`. Its item atlas and some icons are extracted from the game
and are not covered by that MIT grant, as the upstream README states.
