# nx32recomp

Static recompilation of **32-bit (AArch32) Nintendo Switch games** to native Windows executables.

Existing Switch recompilers only handle AArch64. Some Switch titles, notably early ones and ports, are 32-bit
(`is64bit = 0` in their NPDM). nx32recomp covers them:

- **Recompiler** (`tools/recomp/`): ARM (A32) → C. It handles functions, jump tables, VFP and NEON.
  It is checked instruction by instruction against Unicorn (`test_diff.py`).
- **Horizon runtime** (`runtime/`): SVCs, CMIF IPC and the services a game needs. These are fs (RomFS, saves and
  LayeredFS-style mods), hid (SDL3 gamepads), audio (audren → SDL3), vi, am, acc, set, time, pl and mii. Videos go
  through `movie::Decoder` with Media Foundation.
- **GPU**: the game's own NVN driver runs as recompiled code. Its nvdrv commands go to
  [Eden](https://git.eden-emu.dev/eden-emu/eden)'s `video_core` (Vulkan), built as `eden_gpu.dll` (`eden_gpu/`). An
  optional HDR10 output is included.

Code comments are in French.

Written with AI assistance (Claude). Not affiliated with Nintendo or with the Eden project.

## Legal

This repository contains **no game code, data or keys**. Never commit or distribute what it produces from a game:
- the linked image, `build/` and generated C;
- the per-game profile (`profil/<game>/`, which holds addresses in the game binary);
- the game plugin, its RomFS and system archives.

Use your own console and your own legally obtained copy of the game.

## How it works

```
exefs/ (main, sdk, subsdk*, rtld)          your game's ExeFS, extracted with your keys
  └─ tools/link.py exefs build/lien         static link: build/lien/image.bin + layout.json
  └─ tools/recomp/analyse.py <module> build/<module>_funcs.json   (for each module)
  └─ tools/recomp/gen.py exefs build        C code in build/gen (NX_PROFIL_JEU: hle.txt, seeds)
  └─ tools/recomp/mk_entry.py build         build/gen/entry.h (startup symbols, module constructors)
  └─ cmake -DNX_JEU_DIR=<game plugin> -DNX_EXE=<name> ...   → <name>.exe + eden_gpu.dll + SDL3.dll
```

At runtime, the executable expects these data files:
- `donnees/romfs.bin`: the raw RomFS (`hactool --romfs`).
- `donnees/systeme/<id>.romfs`: system archives, extracted from your NAND with `tools/systeme.py`.
- `donnees/polices/`: shared fonts, extracted with `tools/romfs.py`.
- `donnees/image.bin`: the linked image.

`NX_DONNEES` changes the data folder.

### Game plugin

Everything that belongs to one title goes through `runtime/jeu.h`, implemented by a static library outside this
repository. `jeux/exemple/` is a neutral template. The plugin provides:
- the name and title ID;
- data patches applied after loading;
- native replacements registered by address;
- RomFS files rewritten on the fly;
- extra ini sections;
- optionally, the guest addresses that the native video decoder needs (`jeu_adresses_movie`). Without them, the
  game's own `movie::Decoder` runs, and it needs the hardware decoder, which is not emulated.

### Profile (`NX_PROFIL_JEU`)

The profile is a folder with two kinds of files:
- `hle.txt` lists the functions replaced natively. Entries are SDK symbols or game addresses. Start from
  `profil/exemple/hle.txt`.
- `<module>_seeds` files are named `<module>_graines.txt`. They hold extra function entry points found at runtime
  (`build/manquantes.txt` → `tools/recomp/graines.py`).

## Configuration

`<exe>.ini` next to the executable is created on first launch. It has these sections:
- `[fenetre]`: window;
- `[affichage]`: internal resolution, filter, anti-aliasing, anisotropy, vsync, aspect ratio, HDR;
- `[manettes]`: gamepads;
- `[systeme]`: language, region, nickname;
- `[mods]`;
- the plugin's own sections.

Diagnostics use `NX_*` environment variables, for example `NX_SVC_TRACE`, `NX_IPC_TRACE`, `NX_DUMP_THREADS`,
`NX_EDEN_VALIDATION` and `NX_NVN_TRACE_ORIGINE`.

## Building

See `docs/DEPENDANCES.md`. In short:
1. Fill `tiers/` with Eden at the pinned commit, with `eden_gpu/eden.patch` applied, and the other third-party
   folders.
2. Run `python tools/eden/gen_shaders.py`.
3. From a Visual Studio x64 prompt with LLVM in `PATH`:

```
cmake -S . -B build/cmake -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release ^
      -DNX_JEU_DIR=<plugin> -DNX_EXE=<name> -DNX_BUILD=<build folder> -DNX_TIERS=<tiers folder>
cmake --build build/cmake
```

## Status

- Developed against one commercial 32-bit title, which is playable from boot to races, including 4-player split
  screen, at 60 fps with internal resolution scaling.
- Windows x64 only.
- Known limits: the time zone is fixed to Europe/Paris (`time.cpp`); there is no online play and no hardware video
  decoding (videos go through Media Foundation instead).

## License

GPL-3.0-or-later. `eden_gpu` is derived from Eden, which is GPL-3.0-or-later, and the executable links it.
