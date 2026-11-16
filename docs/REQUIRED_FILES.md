# Required files

Everything comes from **your own** PS3 and **your own** copy of the game.
Nothing here is in the repository, and nothing the build produces from it may
be shared.

## The game

- **MotorStorm Apocalypse, BCES-00484 (Europe), version 01.00**, dumped from
  your disc. Only this release is supported: the port's addresses are those
  of its executable. Updates 1.01/1.02 (`ms3patchv0101.psarc`,
  `ms3patchv0102.psarc`) are **not** used. The game looks for them and
  carries on without them, as a PS3 with no update would.
- The whole `PS3_GAME/` folder of the dump (~19 GB, most of it
  `USRDIR/ms3.psarc` and the videos in `USRDIR/ui/video`).

## The executable

`PS3_GAME/USRDIR/EBOOT.BIN` is encrypted. Decrypt it to `EBOOT.elf`, for
example with RPCS3 (*Utilities → Decrypt PS3 Binaries*), with the game's own
disc in place. The toolkit's `unfself.py` cannot do it: it only unwraps debug
(fake) SELFs, and the retail EBOOT uses key revision `0x000D`.

The result must be:

```
sha256 387c9fb795e1709bd2cf1a1145c0548a0fc911a295aa860a27b41441a15418ff  EBOOT.elf
size   21318304 bytes
```

`scripts/lift.sh` checks this and warns if it does not match.

## Layout

`disc/` is git-ignored. Copy the dump there, or link it:

```
disc/PS3_GAME/
├── PARAM.SFO, ICON0.PNG, PIC0.PNG, PIC1.PNG, SND0.AT3, ...  (from the dump)
└── USRDIR/
    ├── EBOOT.elf     # the decrypted executable (a real file)
    ├── ms3.psarc     # from the dump
    ├── budgets/      # from the dump
    └── ui/           # from the dump
```

Linking the dump instead of copying it (adjust `SRC`):

```sh
SRC=/path/to/your/dump/PS3_GAME
mkdir -p disc/PS3_GAME/USRDIR
for f in "$SRC"/*; do [ "$(basename "$f")" = USRDIR ] || ln -s "$f" disc/PS3_GAME/; done
for f in "$SRC"/USRDIR/*; do [ "$(basename "$f")" = EBOOT.BIN ] || ln -s "$f" disc/PS3_GAME/USRDIR/; done
cp /path/to/EBOOT.elf disc/PS3_GAME/USRDIR/
```

### The game somewhere else

`disc/` is only the default. When the port starts in its window and something
it needs is not there, it shows a set-up screen instead of the game: what was
found, what is missing and how to get it, and a button to choose the folder of
your dump (the folder holding `PS3_GAME`; `PS3_GAME`, its `USRDIR` or the
`EBOOT.elf` in it work too). The choice is kept in `user/settings.ini`
(`[game] path`). The order: the path given to the binary when it exists, then
`PS3_GAME_DIR`, then that setting, then `disc/`. The folder only moves the
game's data and its `EBOOT.elf`: the code is in the port, built from the one
executable above.

## Firmware

Reads from `/dev_flash` (fonts and system files) come from RPCS3's install of
the official PS3 firmware (PUP), as in the inFAMOUS 2 port.
