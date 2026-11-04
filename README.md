<h1 align="center">MotorStorm Apocalypse — native PC port (Linux, Vulkan)</h1>

<p align="center">
  <img src="https://img.shields.io/badge/status-early%20development-orange" alt="Status: early development">
  <img src="https://img.shields.io/badge/platform-Linux%20x86--64-1793D1?logo=linux&logoColor=white" alt="Linux x86-64">
  <img src="https://img.shields.io/badge/GPU-Vulkan-AC162C?logo=vulkan&logoColor=white" alt="Vulkan">
  <img src="https://img.shields.io/badge/PS3-BCES00484-003791?logo=playstation&logoColor=white" alt="PS3 BCES00484">
  <img src="https://img.shields.io/badge/code-MIT-blue" alt="Code MIT">
</p>

<p align="center"><b>MotorStorm Apocalypse PC port</b> · native on Linux · PS3 static recompilation · Vulkan</p>

**MotorStorm Apocalypse** (PS3, BCES-00484, version 01.00) running natively on
Linux, without an emulator. The game's code, PowerPC (PPU) and SPU, is
translated to C before compilation by static recompilation with
[ps3recomp](https://github.com/sp00nznet/ps3recomp). The PS3 system is
reimplemented, and the GPU (RSX) runs the game's own shaders translated to
Vulkan. The runtime started as a copy of the
[inFAMOUS 2 port](../infamous2-pc)'s.

> **No game files come with this project.** To play you need your own copy,
> dumped from your PS3. The build turns **your** executable into code on
> **your** machine, and that code cannot be distributed.

## Status

| Area | State |
|---|---|
| Lift (PPU + SPU) and link | ✅ 76,372 PPU functions, 58 SPU images; reaches the entry point |
| Boot, videos, menus | ✅ Boots to the main menu, navigable with a pad (movies play black: no video decoder yet) |
| Races, all single-player modes | ⬜ |
| Split-screen (2–4 players) | ⬜ |
| Audio, saves, controllers | ⬜ |
| Port menu, options (F1), up to 4K, unlocked 60+ fps simulation | ⬜ |
| Online play | ❌ Not planned (the servers are gone) |
| Windows | ❌ Not yet: the Vulkan backend is Linux-only |

Plan and progress: [docs/ROADMAP.md](docs/ROADMAP.md). Technical notes:
[docs/NOTES.md](docs/NOTES.md).

## Required game files

See [docs/REQUIRED_FILES.md](docs/REQUIRED_FILES.md): the BCES-00484 01.00
disc dump, its `EBOOT.BIN` decrypted to `EBOOT.elf`, and the PS3 firmware
installed in RPCS3.

**PC:** Linux x86-64, a CPU with AVX2 (AVX-512 preferred for the SPU code),
a Vulkan GPU, ~14 GB of free RAM to compile, and disk space for the
translated code.

## Building and playing

```sh
scripts/setup.sh    # ps3recomp at the pinned commit + this port's patch, Python venv
scripts/lift.sh     # translates the PPU and SPUs of YOUR EBOOT.elf
scripts/build.sh    # compiles into work/port/build-vk
work/play.sh        # window + controller + sound
```

## Credits

See [CREDITS.md](CREDITS.md).

## License

MIT for the code in this repository; see [LICENSE](LICENSE). MotorStorm
Apocalypse and its content belong to their owners (Evolution Studios / Sony
Interactive Entertainment). This is a research and preservation project.
