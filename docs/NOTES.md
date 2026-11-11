# Technical Notes

## MotorStorm Apocalypse PS3 Port Architecture

### Game Identification

- **Title**: MotorStorm Apocalypse
- **Platform**: PlayStation 3
- **Region Codes**: 
  - BLUS30353 (North America)
  - BCES00484 (Europe)
  - BCAS00051 (Asia)
  - BCJS70015 (Japan)
- **CPU**: Cell Processor (PowerPC PPU + SPUs)
- **GPU**: RSX Reality Synthesizer

### Recompilation Process

The port uses ps3recomp to:

1. **Lift PPU Code**: Translate PowerPC executable to C code
   - Located in: `work/src/recomp/`
   - Generated from: `disc/PS3_GAME/USRDIR/EBOOT.elf`

2. **Lift SPU Code**: Extract SPU modules and jobmods
   - Located in: `work/port/spu/`
   - Includes audio, decompression, and custom job modules

3. **Port Graphics**: Translate RSX shader assembly to Vulkan
   - Handles: Vertex shaders, Fragment shaders, Rasterizer state
   - Target: Vulkan API for Linux GPU acceleration

### Directory Organization

```
motorstorm_apoca/
├── disc/              - PS3 disc files (game data)
├── gamedata/          - HDD save data
├── hdd0/              - Virtual HDD mount point
├── work/
│   ├── src/
│   │   └── recomp/    - Lifted PPU C code (GENERATED)
│   ├── port/          - Port-specific C++ implementation
│   │   ├── main.cpp   - Entry point
│   │   ├── ppu_native.cpp - Native optimization passes
│   │   ├── spu/       - SPU image database
│   │   └── build-vk/  - Vulkan build output
│   ├── tools/         - Utility scripts
│   ├── tests/         - Test runners
│   ├── patches/       - ps3recomp modifications
│   └── out/           - Intermediate files
├── ps3recomp/         - ps3recomp toolkit submodule
└── rpi/               - Research project items
```

### Key Files

- `work/port/CMakeLists.txt` - Build configuration
- `work/port/config.toml` - Runtime configuration
- `work/port/main.cpp` - Main entry point
- `work/port/ppu_native.cpp` - Hand-optimized functions
- `work/patches/ps3recomp.patch` - Toolkit modifications

### Build Process

1. **Setup**: `scripts/setup.sh`
   - Fetches ps3recomp at base commit
   - Applies game-specific patch
   - Installs Python dependencies in `.venv`

2. **Lift**: `scripts/lift.sh`
   - Validates EBOOT.elf hash
   - Generates PPU C code with ppu_lifter.py
   - Extracts SPU modules with lift_spu.py
   - Creates native function stubs

3. **Build**: `scripts/build.sh`
   - CMake configuration with Vulkan enabled
   - Compiles PPU code (in chunks to manage memory)
   - Links ps3 system library implementations
   - Produces `work/port/build-vk/MyGameRecomp` binary

### Runtime System

The port reimplements PS3 system libraries:

- **lv2**: Kernel-level services (threads, synchronization)
- **sys_net**: Network stack (sockets, DNS)
- **cellPad**: Input handling
- **cellAudio**: Audio output
- **cellFs**: File system access
- **cellRtc**: Real-time clock
- **cellGcmSys**: Graphics context management

### Graphics Pipeline

RSX shaders are translated to Vulkan:

1. Input: PowerPC/SPU shader binaries
2. Disassembly: Extract shader operations
3. Translation: Convert to GLSL/SPIR-V
4. Compilation: Vulkan shader module creation
5. Execution: GPU rendering via Vulkan

### Performance Considerations

- PPU code compiled to native x86-64
- SPU jobs either lifted or emulated (configurable)
- Memory layout matches PS3 but runs in Linux process space
- Vulkan for graphics reduces CPU overhead vs. emulation

### Limitations and Known Issues

- SPU lifting may have accuracy issues with complex job modules
- Some RSX features may not have perfect Vulkan equivalents
- Save data format requires conversion
- Online multiplayer not supported

### Testing

Test scripts available in `work/tests/`:

```bash
work/tests/run_all.sh          # Run all tests
work/tests/first_draw.sh       # Test rendering
scripts/build.sh --tests       # Build test suite
```

### Contributing

When adding port features:

1. Keep lifted code untouched
2. Add native implementations in `work/port/ppu_native.cpp`
3. Document shader changes in code comments
4. Test on different GPU vendors (NVIDIA, AMD, Intel)
5. Keep memory usage reasonable (no swap)

### References

- [ps3recomp](https://github.com/sp00nznet/ps3recomp) - Recompilation framework
- [RPCS3](https://rpcs3.net) - PS3 emulator (behavioral reference)
- [Vulkan Spec](https://www.khronos.org/vulkan/) - Graphics API

## SPU side

What runs on the SPUs, how each piece is started, and how the port lifts and
dispatches it. Addresses are EAs in BCES-00484 01.00's EBOOT.elf (segment 0:
EA = file offset + 0x10000) unless marked LS. Everything here is found by
`work/tools/lift_spu.py` by scanning; it writes `work/port/spu/` (git-ignored)
and `work/port/spu/spu_manifest.json`, which lists every image below with its
kind, EA, size, entry, image id and fingerprint.

### What there is

All SPU code is linked into the EBOOT. There are four kinds:

1. **WWS Job Manager policy module** (1). It is a SPURS workload.
   `fn 0x86B3C0` ("Wws_Job", `job/src/ppu/jobapi/jobarraycontainer.cpp`) calls
   `_cellSpursWorkloadAttributeInitialize` and then `cellSpursAddWorkloadWithAttribute`.
   It reads the module's EA and size through two TOC getters at 0x86CF70.
   The EA word (0x14421A4) is 0x1396100. The size word (0x14421A8) is 0x2F10,
   and the getter returns it as `(size+127)&~127` = 0x2F80. The SPURS kernel
   (`libs/spurs/spurs_kernel.c` `kdispatch`) copies 0x2F80 bytes to LS 0xA00
   and looks up the lift by the FNV of those bytes, so the fingerprint covers
   0x2F80 bytes, not 0x2F10. This PM is a newer build than inFAMOUS 2's.
   inFAMOUS's is 0x2E80 bytes and its code is shifted by +0x40 after an early
   insertion. Its data layout is the same: `stqa 0x1320` (job-code base),
   0x1330, 0x12E0, `bufferSetArray` at 0xDF0, the 0xC00 command fetch, and the
   0xA2C service and interrupt entry. So the runtime's image-2 WWS handling
   (`spu_channels.c` overlay match through LS[0x1320], the job return to LS 0,
   `pm_end`) applies unchanged. It is registered as image 2.
2. **WWS job modules** (49, `4 ila | entry | size | 0 | 0 | C0DEC0DE`). The PM
   streams them into its job-code buffer. They are uncompressed in segment 0,
   from 0x12E5880 to 0x13D03C0.
   - 39 are named by a `{char* name, u32 ea, u32 size}` table at rodata
     0x10F7468 (size = header image size - 32): particles, vehicle physics,
     deformation, renderables, buckets, occlusion/PVS, ground rush, post
     effects and so on.
   - 10 more have entry +0x30 and sit around the PM, from 0x1393880 to
     0x13BD100. None is in the name table. Eight are reached through TOC
     entries (0x14425FC to 0x144292C) from PPU functions 0x882AD4 to 0x88DDB8.
     Those functions use the strings `acm%d`, `VoiceCount`, `DlsBankAssign`,
     `Tempo`, `BpmScalar` and `Pitch`, so these are the audio engine's synth
     jobs. The engine looks like Evolution's "Amadeus" (`AmadeusSpuModule`,
     `AcSynthCode`, `data/amadeus/*.wacd`). 0x1393880 and 0x13BD100 have no
     direct pointer; 0x13BD100 carries the same strings as 0x13B0480.
   - No two modules share a 16-byte header, which is how the runtime tells
     them apart. None is byte-identical to an inFAMOUS 2 module.
3. **SPURS taskset tasks** (8 SPU ELFs, linked at LS 0x3000 with `.SpuGUID`).
   cellSpurs fingerprints the task ELF over `spu_elf_image_size()` bytes.
   - The five Havok tasks have the note `../Lib/spu_gcc_4-1-1/release/hk{a,p}`
     and are named after their hkMonitor timers. `hkSpuUtil`
     (`hkPs3SpuUtil.cpp`, "HavokSpursTaskset") starts them with
     `cellSpursCreateTaskset2` and `cellSpursCreateTask2` (fn 0x8C06FC).
   - MultiStream, statically linked libmstream (note `spu/msngSPURS_MP3`),
     starts its task with `cellSpursCreateTaskset` and `cellSpursCreateTask`
     (call 0x8469A4). The ELF pointer is the data word at 0x145FCD4.
   - EDGE post MLAA: the TOC entry at 0x144211C, created with
     `cellSpursCreateTaskWithAttribute` (call 0x858250).
   - EDGE zlib inflate: the TOC entry at 0x14429AC, `edgezlib_inflate_queue.cpp`
     on an LFQueue, created with `cellSpursCreateTask` (call 0x9B64AC). It is
     byte-identical to inFAMOUS 2's (same fingerprint), so it has already run
     in that port.
4. **MultiStream DSP effect plugins** (17 SPU ELFs named `mstream_dsp_*.pic`,
   linked at 0x80, position-independent, from 0x1382300 to 0x1392E80). The PPU
   passes them to `cellMSDSPLoadDSPFromMemory` (statically linked), and the
   MultiStream SPU core copies each into its own local store at an address it
   chooses at run time. **They are not lifted** (see open questions).

Not present (the script checks the first two and fails if they appear):

- **SPURS job chains.** There is no `cellSpursCreateJobChain*` import and no
  inFAMOUS-style `0x44012850` job binaries or `0xDA7ABA5E` job plugins.
- **Raw SPUs and SPU thread groups.** No `sys_spu_thread_group_create` (170) or
  `sys_spu_thread_initialize` (172) syscall appears anywhere. The only SPU
  syscalls are 169, 171, 177, 181, 182 and 190, plus `sys_spu_image_open` (156)
  and `..._open_by_fd` (260) inside the cellFs wrapper layer at 0x9D34B0, which
  nothing appears to call for an SPU program.
- **SPU code in data files.** ms3.psarc (zlib, 8420 entries) was checked by
  reading its TOC, the manifest and the first block of every entry, without
  extracting anything. No entry is an SPU ELF or contains `\x7FELF` or
  `C0DEC0DE`. The 44 `*.bin` files are track data (`/data/track_*/*.bin`), and
  `%s.bin` in the EBOOT is the track-file pattern. USRDIR holds nothing else.

### How each is lifted and registered

`lift_spu.py --toolkit DIR --eboot ELF --spu-dir work/port/spu` (from
`scripts/lift.sh`). It needs `work/out/EBOOT.{imports,loader}.json` from
`ppu_loader.py`. It lifts with `--local-regs --merge-chunks --const-bi`, as
the inFAMOUS port does, and `SPU_LIFT_EXTRA` adds more arguments.

- **PM.** Wrapped with `tools/wrap_spu_elf.py` at LS 0xA00 and lifted into
  `wws_pm/`. It is registered in `spu_workloads.c` as image 2 under fp
  0xFE229D83A0B8B05D. Extra entries are derived from the PM's code:
  - the target of the `bra` it assembles at LS 0 (`ilhu 0x180; iohl imm; shli 5`);
  - the return point after each `bisl $r0` (a call into the staged job);
  - the return point of each `brsl` that follows before the next branch.

  This gives 0xA2C, 0x32B0, 0x32B8, 0x32C0, 0x3674 and 0x369C. On inFAMOUS's
  PM the same rule yields that port's hand-found list (0xA2C, 0x3218, 0x3220,
  0x3228) plus 0x35DC and 0x35FC.
- **Job modules.** Extracted with `tools/extract_jobmods.py --elf` and lifted
  with `tools/lift_jobmods.py` (base LS 0x4000). They are registered in
  `jobmods_register.c` as images 200 to 248, by 16-byte signature.
- **Tasks.** Lifted with `tools/build_spu_workloads.py` into
  `task_<name>_<EA>/`. They are registered in `spu_elf_workloads.c` as images
  30 to 37, by ELF fingerprint and by `.text` fingerprint. Each code segment
  is registered as a region at its link LS address (13 regions; with the 49
  signatures that is 62 of the runtime's 128 overlay slots).
- **DSP plugins.** `MS_DSP_LIFTS` in the script names the plugins to lift
  and the LS address each runs at (images 260 and up, registered with
  `spu_overlay_register_region_at`); the rest stay on the interpreter.
- `NATIVE_ENTRIES` is empty.

**Compile check.** Each of the 58 lifted TUs and the 3 registration TUs was
compiled alone with clang 18:
`-std=gnu17 -O2 -march=x86-64-v3 -DSPU_USE_DISPATCH_MT -DSPU_WRCH_INLINE
-DSPU_LS_NO_DEBUG`, `-I` for ps3recomp/runtime/{spu,ppu,memory,syscalls,platform},
include, libs/video and work/port, then `-c -o /dev/null`. All 61 compile; the
slowest, the MultiStream core, takes 139 s.

A second run gives a byte-identical tree. A run whose lift fails stops with
`[lift_spu] ERROR: ...` and exits 1.

Some lifts have `.word` counts: undecodable words inside detected functions,
mostly rodata linked into the executable segment. In job modules there are
127, in posteffectsjob (27), vehicledeformation (4) and evaluatelightflicker
(96). The tasks have between 26 and 728 each; the most is the stripped
MultiStream core. The PM has 4. They matter only if executed; the lifter
leaves them to the interpreter.

### Images
| Image | EA | Bytes | Entry (LS) | Image id | Fingerprint (FNV-1a-64) | Launched by |
|---|---|---|---|---|---|---|
| `wws_pm` | `0x1396100` | `0x2f80` | `0xa00` | 2 | `0xFE229D83A0B8B05D` | cellSpursAddWorkloadWithAttribute; kernel copies to LS 0xA00 |
| `task_edge_post_mlaa_012D3B00` | `0x12d3b00` | `0x8854` | `0x3050` | 30 | `0xABB700B72A7C5911` | cellSpursCreateTask* (taskset task) |
| `task_edge_zlib_inflate_012DC380` | `0x12dc380` | `0x94d4` | `0x3050` | 31 | `0x00E729804474449A` | cellSpursCreateTask* (taskset task) |
| `task_hka_sample_anim_0121B900` | `0x121b900` | `0xd8b0` | `0x3070` | 32 | `0x4E98340C866A9979` | cellSpursCreateTask* (taskset task) |
| `task_hkp_collide_01246700` | `0x1246700` | `0x25eb0` | `0x3070` | 33 | `0x8CAA5FBF46850CA6` | cellSpursCreateTask* (taskset task) |
| `task_hkp_collision_query_0126C600` | `0x126c600` | `0x27690` | `0x3070` | 34 | `0xBAB337C17D4FE548` | cellSpursCreateTask* (taskset task) |
| `task_hkp_integrate_01229200` | `0x1229200` | `0x1d4c0` | `0x3070` | 35 | `0xFB6C0695EC742787` | cellSpursCreateTask* (taskset task) |
| `task_hkp_raycast_01293D00` | `0x1293d00` | `0x2879c` | `0x3070` | 36 | `0xC33D377E7CC64352` | cellSpursCreateTask* (taskset task) |
| `task_multistream_mp3_012BC500` | `0x12bc500` | `0x17594` | `0x3050` | 37 | `0x57B37E2E758AFC4C` | cellSpursCreateTask* (taskset task) |

Job modules (all lifted at LS 0x4000, registered by their 16-byte header). `-` = not in the name table:

| Image id | EA | Init bytes | Entry (LS) | Lifted dir (`jobmod_<sha1[:12]>_e<entry off>`) | Name | Funcs | `.word` |
|---|---|---|---|---|---|---|---|
| 211 | `0x12e5880` | `0x21a0` | `0x5dc0` | `jobmod_3a8a6f2b5571_e1DC0` | step1module | 164 | 0 |
| 215 | `0x12e7a80` | `0x11a0` | `0x4e18` | `jobmod_44afad2d207b_e0E18` | step2module | 162 | 0 |
| 233 | `0x12e8c80` | `0x2140` | `0x5c68` | `jobmod_be7b1040dec1_e1C68` | vehicledeformation | 228 | 4 |
| 230 | `0x12eae00` | `0x18a0` | `0x54a0` | `jobmod_9eb95f7382ef_e14A0` | vehicleconstraints | 148 | 0 |
| 235 | `0x12ec700` | `0x1b20` | `0x4ef8` | `jobmod_c3c80fee2d40_e0EF8` | evaluatelightflicker | 159 | 96 |
| 225 | `0x12ee280` | `0x1af0` | `0x56a0` | `jobmod_937cadf72eb2_e16A0` | CullEmittersModule | 138 | 0 |
| 238 | `0x12efd80` | `0x16a0` | `0x5328` | `jobmod_ceb61000674c_e1328` | updategameemitters | 176 | 0 |
| 203 | `0x12f1480` | `0x13a0` | `0x4fb8` | `jobmod_081f2525ba20_e0FB8` | ParticleFillLightingModule | 141 | 0 |
| 202 | `0x12f2880` | `0xef0` | `0x4b18` | `jobmod_0521e23a02a2_e0B18` | CullEffectorsModule | 122 | 0 |
| 219 | `0x12f3780` | `0x1a20` | `0x5600` | `jobmod_68fb1d8b692a_e1600` | BasicParticleFillerModule | 202 | 0 |
| 242 | `0x12f5200` | `0x1b20` | `0x5710` | `jobmod_e9cf10167d5d_e1710` | Particle3DFillerModule | 204 | 0 |
| 218 | `0x12f6d80` | `0x1ca0` | `0x5838` | `jobmod_5a34b23a413c_e1838` | OrientatedParticleFillerModule | 204 | 0 |
| 245 | `0x12f8a80` | `0x1ba0` | `0x5760` | `jobmod_faae684ab9b4_e1760` | OrientatedParticle3DFillerModule | 202 | 0 |
| 217 | `0x12fa680` | `0x1ba0` | `0x5778` | `jobmod_4b47e08b3a04_e1778` | OrientatedStretchedParticleFillerModule | 202 | 0 |
| 207 | `0x12fc280` | `0x1ba0` | `0x5768` | `jobmod_1d85492a415e_e1768` | OrientatedStretchedParticle3DFillerModule | 202 | 0 |
| 216 | `0x12fde80` | `0x1d20` | `0x5950` | `jobmod_4b11cf1bd2e0_e1950` | Ribbon2DParticleFillerModule | 209 | 0 |
| 228 | `0x12ffc00` | `0xc20` | `0x4890` | `jobmod_9b53a9b443e9_e0890` | ParticleSortModule | 117 | 0 |
| 223 | `0x1300880` | `0x91c0` | `0xc3f0` | `jobmod_879c21e9ceb9_e83F0` | ProcessEmittersModule | 384 | 0 |
| 200 | `0x1309a80` | `0x1f70` | `0x5b40` | `jobmod_03392e9987d2_e1B40` | ProcessEffectorsModule | 152 | 0 |
| 236 | `0x130ba00` | `0x1ba0` | `0x57d0` | `jobmod_c6c18049adda_e17D0` | spupathfinding | 182 | 0 |
| 244 | `0x130d600` | `0xb1c0` | `0xebf8` | `jobmod_f5fc590a4369_eABF8` | occjob | 608 | 0 |
| 239 | `0x1318800` | `0xbc20` | `0x7ea0` | `jobmod_d55c1580490d_e3EA0` | pvsjob | 479 | 0 |
| 205 | `0x1324480` | `0x3da0` | `0x79c8` | `jobmod_0d0c57598923_e39C8` | groundrushjob | 225 | 0 |
| 208 | `0x1328280` | `0xf20` | `0x4b60` | `jobmod_1fd0c1dd0d33_e0B60` | groundrushvisibilityjob | 127 | 0 |
| 222 | `0x1329200` | `0x11a0` | `0x4df0` | `jobmod_7e98d0e21e99_e0DF0` | groundrushsquashjob | 120 | 0 |
| 226 | `0x132a400` | `0x2b80` | `0x6620` | `jobmod_95e1145299a3_e2620` | screenrushjob | 150 | 0 |
| 248 | `0x132cf80` | `0x15ea0` | `0x76a0` | `jobmod_fd9c2ec37440_e36A0` | render_sortandprocessbucketsjob | 1171 | 0 |
| 241 | `0x1342e80` | `0x18ea0` | `0x8e28` | `jobmod_d78d63199b24_e4E28` | renderablejob | 1398 | 0 |
| 221 | `0x135bd80` | `0x9520` | `0x7888` | `jobmod_7b532b671379_e3888` | render_fillbucketsjob | 361 | 0 |
| 247 | `0x1365300` | `0x2820` | `0x6330` | `jobmod_fc761a281f56_e2330` | generatefogtable | 220 | 0 |
| 231 | `0x1367b80` | `0x1520` | `0x4ff0` | `jobmod_b6a477793f15_e0FF0` | octreequery | 149 | 0 |
| 240 | `0x1369100` | `0x15a0` | `0x51c8` | `jobmod_d6cae8b8000a_e11C8` | dxtblendjob | 123 | 0 |
| 243 | `0x136a700` | `0x1320` | `0x4ef8` | `jobmod_f39ff4434550_e0EF8` | trailsupdate | 176 | 0 |
| 220 | `0x136ba80` | `0x9ed0` | `0xd968` | `jobmod_74ff2bd663f0_e9968` | vehiclephysics | 496 | 0 |
| 227 | `0x1375980` | `0x6220` | `0x5900` | `jobmod_9a05598137db_e1900` | hierarchyevaluate | 198 | 0 |
| 209 | `0x137bc00` | `0x1220` | `0x4e30` | `jobmod_211c1e4effa7_e0E30` | skinnedobjectdeform | 129 | 0 |
| 213 | `0x137ce80` | `0x3620` | `0x4a28` | `jobmod_4283f41cb513_e0A28` | posteffectsjob | 510 | 27 |
| 212 | `0x1380500` | `0xb20` | `0x4788` | `jobmod_3bdd3158a9f5_e0788` | ragdollcontrollerjob | 114 | 0 |
| 224 | `0x1381080` | `0x1220` | `0x4ad0` | `jobmod_91ab790aede6_e0AD0` | animatedobject_updateshadermatrices | 127 | 0 |
| 214 | `0x1393880` | `0x2810` | `0x4030` | `jobmod_44aa644d494e_e0030` | - | 294 | 0 |
| 237 | `0x1399300` | `0xf80` | `0x4030` | `jobmod_cb403075f2f9_e0030` | - | 56 | 0 |
| 234 | `0x139a300` | `0x1180` | `0x4030` | `jobmod_c3388c1f148b_e0030` | - | 60 | 0 |
| 206 | `0x139b500` | `0x3730` | `0x4030` | `jobmod_135114c81d32_e0030` | - | 815 | 0 |
| 210 | `0x139ec80` | `0x5e00` | `0x4030` | `jobmod_2a494cfd289d_e0030` | - | 240 | 0 |
| 232 | `0x13a4b00` | `0x3f80` | `0x4030` | `jobmod_bbd06cc5c063_e0030` | - | 150 | 0 |
| 201 | `0x13a8b00` | `0x3a80` | `0x4030` | `jobmod_0487be1de1c7_e0030` | - | 171 | 0 |
| 204 | `0x13ac600` | `0x3e00` | `0x4030` | `jobmod_08bd99acc630_e0030` | - | 137 | 0 |
| 229 | `0x13b0480` | `0xcc00` | `0x4030` | `jobmod_9c4578e272ec_e0030` | - | 496 | 0 |
| 246 | `0x13bd100` | `0x132a0` | `0x4030` | `jobmod_fb5dc8650b3a_e0030` | - | 834 | 0 |

| DSP plugin | EA | Bytes | .text EA | .text bytes | Entry (link) | Fingerprint |
|---|---|---|---|---|---|---|
| `mstream_dsp_aux_return.pic` | `0x1382300` | `0x868` | `0x1382400` | `0x530` | `0x90` | `0x4DC50B4DEC1EAD20` |
| `mstream_dsp_aux_send.pic` | `0x1382b80` | `0xa98` | `0x1382c80` | `0x790` | `0x90` | `0x9CD2F1ED75E45D40` |
| `mstream_dsp_compressor.pic` | `0x1383680` | `0xf48` | `0x1383780` | `0xc10` | `0x90` | `0x103A2054AAAA08E9` |
| `mstream_dsp_delay.pic` | `0x1384600` | `0x157c` | `0x1384680` | `0x12f0` | `0x98` | `0x750C91DF34EA6F44` |
| `mstream_dsp_distortion.pic` | `0x1385b80` | `0x728` | `0x1385c80` | `0x420` | `0x90` | `0xC53066658B876985` |
| `mstream_dsp_eq.pic` | `0x1386300` | `0x6f8` | `0x1386400` | `0x3c0` | `0x90` | `0x3EC6622CEE1BD1BA` |
| `mstream_dsp_filter.pic` | `0x1386a00` | `0x78c` | `0x1386a80` | `0x500` | `0x90` | `0x7D63C2E2573BC73D` |
| `mstream_dsp_i3dl2.pic` | `0x1387200` | `0x2a4c` | `0x1387280` | `0x27c0` | `0x98` | `0xE8DCECD29500221B` |
| `mstream_dsp_IR.pic` | `0x1389c80` | `0x1158` | `0x1389d80` | `0xe50` | `0x90` | `0x444A324110D39DC9` |
| `mstream_dsp_multi_compressor.pic` | `0x138ae00` | `0x11d8` | `0x138af00` | `0xea0` | `0x90` | `0xE7DC4C487F96FC99` |
| `mstream_dsp_multi_compressor_TD.` | `0x138c000` | `0x1358` | `0x138c100` | `0x1020` | `0x90` | `0x4E306DB8F0D85C1F` |
| `mstream_dsp_para_eq.pic` | `0x138d380` | `0x91c` | `0x138d400` | `0x690` | `0x98` | `0xC7DCAC4C4B256A01` |
| `mstream_dsp_pitchshift.pic` | `0x138dd00` | `0x1c08` | `0x138de00` | `0x1900` | `0x90` | `0x07976FA227C3258B` |
| `mstream_dsp_reverb.pic` | `0x138f980` | `0x220c` | `0x138fa00` | `0x1f80` | `0x98` | `0x77703FBE93B9E6AD` |
| `mstream_dsp_ringmod.pic` | `0x1391c00` | `0xa0c` | `0x1391c80` | `0x7b0` | `0xc0` | `0xB2934B0DD063CDC4` |
| `mstream_dsp_softclip.pic` | `0x1392680` | `0x7a8` | `0x1392780` | `0x4a0` | `0x90` | `0x9B94699D398683C0` |
| `mstream_dsp_vocoder.pic` | `0x1392e80` | `0x9cc` | `0x1392f00` | `0x740` | `0x90` | `0x2123DD90AFD04A9C` |

Lift sizes for the tasks and the PM (functions / `.word`): PM 200/4,
mlaa 959/26, zlib 661/300, hka 670/135, hkp_collide 1741/102,
hkp_collision_query 1698/127, hkp_integrate 1166/91, hkp_raycast 1819/119,
multistream_mp3 1201/728.

### Open questions and missing runtime support

- **The Taskset2 API is not implemented in ps3recomp's HLE.** Eight of the 47
  imported cellSpurs functions have no implementation in `libs/spurs`:
  - `cellSpursCreateTaskset2`, `_cellSpursTasksetAttribute2Initialize`,
    `cellSpursCreateTask2`, `cellSpursJoinTask2`, `cellSpursTryJoinTask2`,
    `cellSpursDestroyTaskset2`: Havok's path, so none of the five Havok tasks
    can start;
  - `cellSpursBarrierInitialize`;
  - `cellSpursLFQueueDetachLv2EventQueue`.

  `cellSpursCreateTask2` has to reach `spu_task_dispatch` with the ELF, as
  `cellSpursCreateTask` does, or the lifted Havok tasks never run.
- **MultiStream DSP plugins.** They are position-independent and loaded at an
  LS address the MultiStream core picks at run time from its own heap, so a
  lift (absolute branch targets) is only valid at one address. In practice
  every run loads the same three at the same places: i3dl2 at LS 0x37000,
  multi_compressor_TD at 0x37480, filter at 0x39800 (found by matching the
  words the `[spu] img=37 branched into unlifted LS` lines print against the
  plugins' .text; 42 logs agree). Those three are lifted there
  (`MS_DSP_LIFTS`); the runtime makes a lift resident when a GET puts its
  bytes at its address and evicts it when a GET overwrites them (the core
  rewrites the first 48 bytes at 0x37000, so i3dl2 is evicted often and runs
  partly interpreted). libmstream itself is statically linked PPU code with
  no HLE, so it runs lifted.
- **Job-code buffer at LS 0x4000?** `lift_jobmods.py` lifts every module at
  0x4000, inFAMOUS's buffer. The PM's data layout matches, but the base it
  stores at LS[0x1320] for this game is unconfirmed. The runtime logs
  `[spu-ovl] WWS job module resident: codeBase=0x.....` on first use.
- **Amadeus jobs.** That the 10 unnamed modules are submitted through the WWS
  PM (`JobHandlerWWSJOB`) is inferred, not traced. 0x1393880 and 0x13BD100
  have no direct reference.
- **Taskset tasks are not registered with `spu_taskset_register_task_entry`
  or `_elf`**, the same as in the inFAMOUS port. That is fine while the HLE's
  `spu_task_dispatch` runs each task in its own local store. It matters if
  the lifted taskset policy (`taskset_policy_bytes.c`) is ever used. That blob
  comes from firmware libsre, which this title does not load, so the file is
  not generated.
- The runtime's hard-coded PM code addresses (0x2318, 0x3258, 0x3308, 0x2F30,
  ...) are inFAMOUS/LBP PM offsets, but they appear only in the
  `SPU_JOBDRAIN` and `SPU_JOBTRACE` diagnostics. With this PM those
  diagnostics point at the wrong instructions.

### EDGE MLAA barrier hang (frame 2) -- fixed

Symptom: boot drew the first frame and the PPU then spun in fn 0x857EF8 on
the MLAA completion word (context 0x40367804 + 0x2C), with the 5 MLAA tasks
of taskset 0x40AD5D00 parked in WAIT_SIGNAL inside the barrier wait.

How the task-side barrier works (image 30, read from the disassembly; the
CellSpursBarrier layout is RPCS3's: +0x00 s32 count, +0x04 total, +0x34
taskset): notify = LS 0xA618, wait = LS 0xA910, both GETLLAR/PUTLLC on the
barrier line (LS 0x80). +0x10 is the 128-bit mask of tasks waiting in the
current phase, +0x20 the mask of tasks that notified during the drain phase.
While count >= 0 notifies count up; the one that makes count == total sets
count = popcount(+0x10) - total and releases every +0x10 task. While count < 0
waits count up to 0 (the -1 -> 0 waiter releases +0x20). A release is the
SPU send-signal routine at LS 0xABF0: GETLLAR on the taskset, refuse
(0x80410905) unless running|ready|pending|signalled|waiting are all inside
`enabled` and running&waiting == ready&pending == 0, then set the task's
`signalled` bit with PUTLLC. Our cellSpursBarrierInitialize was right.

Three bugs, all fixed:

1. `cellSpursCreateTask` did `taskset->taskCount++` on the native
   `CellSpursTaskset` struct, which aliases the guest's BE layout: +0x04 is
   word 1 of `running`. After 5 tasks, running = `00000000 05000000 ...`
   (tasks 37, 39 "running", not enabled), so every SPU-side signal was
   refused and the barrier's waiters never woke. Destroy/Shutdown had the
   same kind of native stores (+0x00, +0x08). All removed (cellSpurs.c).
2. Once signals got through, one task in four still slept: the HLE cleared
   its own `signalled` bit with a plain read-modify-write while the releasing
   task PUTLLC'd the next waiter's bit into the same word, and the stale word
   won. `spurs_bitset_set/clear` (spurs_taskset.h) now run under the line's
   lock-line stripe and call `spu_coh_notify_write` (so a racing PUTLLC
   fails and retries, as against a PPU stwcx.); the task-exit path in
   spu_workload.c uses them too.
3. A guest PUTLLC that sets `signalled` never touched the WAIT_SIGNAL
   condvar, so a parked task only saw it on its 1 s poll. A committed
   PUTLLC/PUTLLUC on a line with a parked taskset now broadcasts it
   (`spu_taskset_line_committed`, called from spu_channels.c).

Test: `libs/spurs/tests/test_spurs_taskset_signal.c` (build line in the file):
5 CreateTasks leave `running` clean and the bitsets consistent, and a
signalled bit set in memory plus the commit hook wakes a parked waiter in
~1 ms.

`SPU_ATOM_EA=<ea>` verdict lines now also print the task id and words
+0x00/+0x10/+0x20 of the committed line, which is what showed bug 2.

After the fix the game flips every frame: the health warning, then the
"PLAY / PLAY IN 3D" menu (engine path, `PS3RECOMP_VK_GUEST_PROGRAMS=1`).
The flip is `_cellGcmSetFlipCommand` on one of four 4 KB per-frame contexts
(0x40B10000..0x40B13000); `gcm_flip_into_fifo` declines them (its 4 KB
RECYCLE_SLACK is the whole segment), so the immediate-flip fallback is what
runs. Fine for now; a FIFO-ordered flip for small contexts is a follow-up.

`LD_FRAME_DUMP` used to be honoured only by the live-draw (D3D12) path, so
`first_frame.sh` found no frames on the Vulkan build even once the game was
flipping. The Vulkan backend now takes it too (`<dir>/frame_<n>.ppm`, the
same `[frame-dump]` line).

Performance note: without `PS3RECOMP_VK_GUEST_PROGRAMS=1` (the tests' default)
the vtable path uploads every bound texture with a blocking submit, ~600 ms
per frame, and draws the frame flipped and offset; with guest programs the
engine runs ~30 fps. The engine path logs `colour target format 10/12 ...
not supported yet` for the post-process chain (1280x1080, 320x180, ...).

## Virtual pads

Unattended runs drive the game with up to four virtual pads instead of a
person (see "Working mode"). The code is in `ps3recomp/libs/input/cellPad.c`.
The older port-0 knobs (`PAD_SCRIPT`, `PAD_FILE`, `PAD_AUTOPRESS`,
`PAD_STICK`) still work as before.

### Environment

| Variable | Effect |
|---|---|
| `PS3_PAD_FILE_<n>=<path>` (n = 0..3) | Held-state file for port n. `PS3_PAD_FILE` is an alias for port 0. |
| `PS3_PAD_SCRIPT=<path>` | Timeline file covering any of ports 0..3. |

Both can be used together. On a port that has both, the file state is
applied first and the script's state on top.

### Which ports are connected

- A port is **virtual** if it has a `PS3_PAD_FILE_<n>` or the script names
  it. A virtual port is pinned to its number and shows as connected in
  `cellPadGetInfo`, `cellPadGetInfo2` and `cellPadPeriphGetInfo` from
  `cellPadInit` on, with or without an SDL pad. The script's `disconnect`
  and `connect` events are the only things that change that.
- **Real pads** (SDL device slots 0, 1, ...) go on the ports that are not
  virtual, lowest port first: slot 0 takes the lowest free port, slot 1 the
  next, and so on. The mapping depends on the slot number, not on which
  slots are plugged in, so unplugging one pad never moves another. If no port
  is virtual, slot n is port n, as before. Rumble goes to the real pad that
  feeds the port.
- Port 0 is still always reported connected, even when it is neither virtual
  nor fed by a real pad: it is the keyboard pad with nothing pressed. Any
  other port with neither is disconnected and returns `len = 0`.

### Held-state file format

The file is re-read on every host poll (every 4 ms). Tokens are separated by
whitespace, `,` or `+`, and names are not case-sensitive.

- `NAME`: the button is held at full pressure. NAME is one of `UP DOWN LEFT
  RIGHT CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 L3 R3 START SELECT`.
- `NAME=v`: pressure v (0-255). The digital bit is set when v > 0. Use this
  for the analog triggers: `R2=200` is part throttle.
- `LX=v LY=v RX=v RY=v`: stick positions, 0-255, centre 128. 0 is left or
  up.

A missing or empty file means nothing is held. On port 0 the file adds to
the keyboard pad, and a file stick at exactly 128 leaves the keyboard's stick
alone. The game only receives pressure values if it turned press mode on
(`cellPadSetPressMode` or `cellPadSetPortSetting` with `PRESS_ON`).
Otherwise `R2=200` arrives as a digital R2 press.

### Script format

One event per line. `#` starts a comment.

```
t=<seconds> port=<n | n,m,... | all> <verb> [arguments]
```

| Verb | Arguments | Effect |
|---|---|---|
| `hold` (or `press`) | controls, `[<seconds>]`, `[polls=N]` | Press the controls, then release them. The press lasts at least `<seconds>` (default 0.1) **and** at least N packets the game received on that port (default 4). |
| `set` | controls | Persistent until changed. `NAME=0` releases that button. |
| `release` | names, or `all` | Removes the named controls from the `set` state and from any hold that is still active. `all` clears everything and centres the sticks. |
| `connect`, `disconnect` | none | Plug or unplug the virtual port. If a port's first event is `connect`, the port starts unplugged. |

Controls use the same tokens as the file. `+` joins several names into one
token: `hold CROSS+START 0.2`. If `port=` is left out, the event goes to
port 0. A line with an error is logged as `[pad] script <path>:<line>:
<reason> -- line ignored` and skipped.

**Clock:** `t` is in seconds since the game's **first `cellPadGetData`
call**, not since process start. The time to boot and load the ELF varies
from run to run, and the first pad poll is the first point where the game is
known to be running. Ending and re-initialising the pad library does not
restart the timeline.

**Latching:** each event fires on the first host poll at or after its `t`,
so an event cannot be missed while the game is not polling, such as during a
loading screen. A hold counts only the packets the game actually read on that
port (`cellPadGetData` returning `len > 0`). If the game stops reading,
the press is still active when it starts reading again.

Events on one port fire in file order. A hold whose controls overlap a hold
that is still active, or one that ended less than 2 packets ago, waits until
that one is done. Later events on the same port wait behind it. This keeps
two scripted taps of DOWN from merging into one long press, which a menu
would read as a single input.

### Log lines

Tests can grep stdout for these lines to check that input reached the game:

```
[pad] PS3_PAD_SCRIPT race4.pad: 16 event(s)
[pad] virtual ports: 0(script) 1(script) 2(script) 3(script); real pads fill the other ports in slot order
[pad] t=0.512 port=1 hold CROSS 0.2 [0.200s, >=4 packets]
[pad] t=0.780 port=1 hold CROSS 0.2 [0.200s, >=4 packets] (due t=0.500)
[pad] t=0.731 port=1 end hold CROSS 0.2 (6 packets)
[pad] t=90.004 port=3 set R2=255
```

There is one line for each event fired, and one line when each hold ends,
with the number of packets the game received during the hold. `(due t=...)`
appears when an event fired more than 50 ms after its scheduled time. The
file pads also log `[cellPad] file pad <n>: buttons 0x....` whenever their
buttons change (port 0 keeps the old wording, without the port number).

### Sample: four-player split-screen race start

```
# race4.pad   run with PS3_PAD_SCRIPT=race4.pad
# Ports 0-3 are all named below, so all four are virtual and connected
# from boot. The menu timings are placeholders until the menu flow
# has been mapped.
t=40    port=0      hold START 0.2           # title screen
t=45    port=0      hold CROSS 0.2           # main menu -> split-screen
t=50    port=1,2,3  hold CROSS 0.2           # players 2-4 join
t=55    port=0      hold CROSS 0.2           # confirm and start the race
t=90    port=all    set R2=255               # full throttle on all four pads
t=95    port=2      set R2=180 LX=100        # player 3: part throttle, slight left
t=96    port=3      hold L2 0.5              # player 4: tap the brake
t=150   port=all    release all
```

To check that throttle reached every port:
`grep -c '^\[pad\] t=.* port=[0-3] set R2=255' run.log` should print 4.

### Unit test

`ps3recomp/libs/input/tests/test_pad_virtual.c` (with `test_pad_stubs.c`)
compiles `cellPad.c` with `-DPS3RECOMP_PAD_TEST_HOOKS`. That build has no
SDL or XInput backend, uses a simulated clock, and can fake real pads in
backend slots. The test runs a 4-port script with a file on port 0 and
checks the following:

- every port's `cellPadGetData` packets for each simulated frame;
- part pressure (R2 = 200);
- two back-to-back taps that do not merge;
- a hold that is due while the port is not being polled and still arrives
  as at least 4 packets;
- hot-plugging with `connect`;
- `release all`;
- the connected bits in GetInfo, GetInfo2 and PeriphGetInfo;
- the `[pad]` log lines;
- the port-assignment rule (virtual port 2 pinned, real slots 0, 1, 2
  going to ports 0, 1, 3);
- the defaults when nothing is configured.

Build and run it from `ps3recomp/`:

```bash
cc -std=gnu17 -O1 -D_GNU_SOURCE -DPS3RECOMP_PAD_TEST_HOOKS -Iinclude \
   -Ilibs/input -Iruntime/ppu -Iruntime/spu -Iruntime/memory \
   libs/input/cellPad.c libs/input/tests/test_pad_virtual.c \
   libs/input/tests/test_pad_stubs.c -lm -o /tmp/test_pad_virtual \
&& /tmp/test_pad_virtual
```

Related fix: when a button's digital bit was set, `cellPadGetData` used to
force its pressure to 255 whenever the value was below 255. It now does that
only when the pressure is 0. The old rule turned any part-pulled trigger,
real or scripted, into full throttle.

## Boot to menu

What MotorStorm Apocalypse does from the first frame to the main menu, and
what had to be fixed on the way (milestone 3). `work/tests/main_menu.sh`
covers all of it.

### The flow

1. HEALTH WARNING, then "This game saves data automatically..." with the
   spinner, then **PLAY / PLAY IN 3D** (stereo select). It waits there for
   CROSS.
2. "Sony Computer Entertainment presents", then `ui/video/evologo_60.avi` and
   `ui/video/maintitles_60.avi`. Each can be skipped with CROSS.
3. Title, "Press Any Button To Continue". Before this the game runs
   `cellSaveDataUserAutoLoad` (dir `BCES00484`) and registers trophies
   (`sceNpTrophy`, `NPWR00899`).
4. **First boot (no save):** SET SCREEN SIZE, then Choose A Control Method,
   then the first `cellSaveDataUserAutoSave` (AUTOSAVE.DAT, about 49 KB, plus
   ICON0.PNG), then *straight into the Prologue race* ("Mash's Prologue",
   `r01_60.avi`). A first boot never shows the main menu.
5. **With a save:** the title goes to the main menu (FESTIVAL / WRECKREATION /
   MY MOTORSTORM / OPTIONS). UP and DOWN move the highlight and the panel on
   the right.

Not used on this path: `cellMsgDialog`, `cellOskDialog` and NpEula (no call
logged in either boot). NP is "signed out": `sceNpInit` is fine, and
`cellNetCtlGetState` answers NOT_INITIALIZED (offline mode), about 7000 times
a boot, even after `cellNetCtlInit`. The game accepts that.

Pads: the game does not import `cellPadSetPressMode`. It turns pressure and
sensors on with `cellPadSetPortSetting(port, 0x6)` (PRESS_ON | SENSOR_ON) on
all ports, so scripted `R2=200` reaches it as analog.

### Movies (cellVdec / cellAdec)

The movies are AVI: XviD video (cellVdec codec type 5, DIVX, 1280x720) and AC3
audio (cellAdec type 2). The demuxer and player are the game's own
statically linked "MoviePlayer" threads (MoviePlayerSourceFile, Demuxer,
videoStream, audioStream, VideoDisplayPS3, AudioDisplayPS3). Three fixes:

- `cellVdecQueryAttrEx`, `cellVdecOpenEx` and `cellAdecOpenEx` were
  unresolved NIDs (0xC982A84A, 0x0053E2D8, 0x8B5551A4). They are now the plain
  forms: TypeEx starts with the same two words as CellVdecType, and ResourceEx
  only adds SPURS placement.
- **Handle 0.** cellVdec handed out slot indices, so the first decoder was
  handle 0. The player keeps the handle at +0xC0 of its decoder object (vtable
  0x1216F30), and its DecodeAu wrapper (fn 0x9F0990, slot 5) returns 155
  without calling `cellVdecDecodeAu` while that is 0. videoStream then waited
  on its decoder event forever, and AudioDisplay waited for the video clock.
  Handles are now slot + 1, as a real handle (a work-memory pointer) is never
  0.
- The single "last picture" became a 16-entry queue. A full queue returns
  BUSY, which fn 0x9F0818 waits on and retries. `CELL_VDEC_ERROR_FATAL` is
  0x80610180 (it was ...0107), the value fn 0x9F0818 compares.

There is still no real decoder: every AU is one black picture, so the movies
play their full length in black (or are skipped with CROSS). libavcodec 60 is
on the build host if real frames are wanted. Test:
`libs/codec/tests/test_vdec_handles.c` (build line in the file).

### The logo that never ends (job-batch event deadlock)

Seen once in a play.sh run (play-20261104-130612.log): evologo_60.avi played,
then black, and maintitles_60.avi never opened. AsyncVidRender (thread 40,
fn 0x4F16DC) went on drawing the last picture, because the main thread, which
sets its stop flag (+0x15C) and joins it, was stuck. The poll sites said how:
thread 1 at 0xFF5B90, thread 3 (JobBatchHandler) at 0xFF5B6C.

The game's event (vtable 0x1201500) is two of its own spinlocks (fn 0x675D3C,
lwarx/stwcx., then sys_timer_usleep(250)): +8 guards it, +12 is the event.
Wait (fn 0xFF5AD8) takes and drops +12. Set(bool) (fn 0xFF5B40) takes +8,
then for true drops +12 and for false (reset) *takes* +12, and only then
drops +8. A reset of an event that is already reset therefore spins on +12
while holding +8, and the Set that would end it spins on +8.

The job batches use it: submit (fn 0x6737E0) resets the batch's event and
queues the batch; JobBatchHandler sets it (fn 0x673BFC) when the batch's SPU
work is done. About ten batches a frame are never waited on (for example the
one queued from 0x61C1C4); the game counts on each being done before the next
frame queues it again. When one is not, the main thread resets it while its
completion is still on the way, and both threads stop for good. It comes down
to timing (a busy host, an SPU job that runs late). Nothing in the window,
present or decoder path is wrong: about a dozen runs (headless, windowed,
through the start screen or not, verbose or quiet) all reached maintitles.

It is reproducible on demand: `work/scratch/movfreeze/forcerun.sh <dir>`
attaches gdb (non-stop) 4 s into the logo and makes that one batch's isDone
(fn 0xFF56E4) say no for 150 ms. Before the fix: the same freeze, same poll
sites. After: the main thread's reset waits out the late Set and the logo ends
into maintitles.

Fix: a native stand-in for fn 0xFF5B40 (`nat_event_set`, work/port/ppu_native.cpp,
listed in work/tools/ppu_native_entries.py). Set(true) stays lifted. The reset
takes +8, tries +12 once, and drops +8 again before backing off, so a late Set
can get in. When the event is set, as it nearly always is, it does what the
guest code does. Each time it has to wait, it logs `[ppu-native] event 0x...
reset while still reset (caller 0x0067382C): waited N x 250 us` (the first
eight): every such line is a freeze the lifted code would not have come out of.
Normal runs print none. `PS3RECOMP_NATIVE_PPU=0` turns it off. The hook is put
in by the post-pass (`ppu_native_entries.py`, which lift.sh runs), so a relift
keeps it.

Two side notes. The window build waits on the port's start screen, and
movrun.sh did not skip it, so the "windowed run is stuck" bisect was a run
that never booted (19 lines of log): movrun.sh now sets PS3_SKIP_LAUNCHER=1.
`PS3_UI_AUTOPLAY_MS=<ms>` (work/port/ui.cpp) presses Play on the start screen
by itself, for a scripted run through it the way play.sh boots
(`work/scratch/movfreeze/arun.sh`).

### Save data

`cellSaveData_register_ctx_handlers()` was never called. Its name begins with
`cell`, so `gen_hle_nids.py` even registered it as a (never imported) guest
function. Without it, `cellSaveDataUserAutoLoad`/`UserAutoSave` took the
8-GPR path, which cannot see the ninth argument (userdata, at caller SP+112),
and passed NULL. The load "worked" (its funcStat ignores userdata on a new
profile), but the game's funcFile reads the file to write from its object at
`cbResult->userdata`: every autosave asked for `name='' buf=0` and failed with
ERROR_PARAM, so no save ever existed and every boot was a first boot.

Now `ps3_savedata_register_ctx()` (libs/system/cellSaveData.c) registers
UserAutoLoad, a new UserAutoSave ctx handler and ListAutoLoad. It is called
weakly from `ppu_hle_init` (runtime/ppu/ppu_hle.cpp), like
`ps3_font_register_ctx`. The first autosave writes `PARAM.SFO`, `PARAM.PFD`,
`AUTOSAVE.DAT` and `ICON0.PNG`, and the second boot loads AUTOSAVE.DAT and goes
to the main menu. Test: `libs/system/tests/test_savedata_fixed.c` now calls
both User* handlers through the registered NIDs and checks userdata. It also
got the stubs it needed to link again (spu_lockline_* masks, gwt_*).

`PS3_SAVEDATA_ROOT` moves the save root. The tests use their own, under
`work/runs/`, so they never touch a player's `gamedata/` saves.

### main_menu.sh

`work/tests/main_menu.sh` runs about 2 minutes and does two boots, each with a
fresh save root. It drives the pad through a `PS3_PAD_FILE` held-state file
from a small controller (Python inside the script), not a timed
`PS3_PAD_SCRIPT`. The movie and load times vary, and a CROSS that lands on the
menu starts the Festival (and the Prologue load).

- Boot 1 taps CROSS every 2 s until `SAVE complete`.
- Boot 2 taps CROSS until the second `[cellVdec] Close` (maintitles). Then
  it looks at the dumped frames: CROSS while the title logo is on screen (mean
  red over x 300..700, y 130..270 is about 90 on the title and 0 on the
  menu), until the menu panel shows. The title ignores the D-pad, and the
  CROSS that skipped maintitles sometimes passes the title already (it reads
  the held button), so a blind CROSS can land on the menu and start the
  Festival. Then DOWN, DOWN, UP: each must change the
  right-hand panel (the highlighted item's picture and text, x 770..1160,
  y 120..490) by at least 5 and by more than 4x its idle change (mean abs
  difference, 0..255). A press measures about 20-45 and idle about 0.1-1.5.
  The item column is not used because the highlighted item pulses between
  white and grey by itself (idle up to about 11).

PASS also requires no `unresolved NID` in either log. The menu frame is
written to `work/runs/main_menu.png`.

### First-boot hdd1 corruption

A player's first boot (empty `/dev_hdd1`, `PS3_HDD1_ROOT`) died on about 5
boots of 6: null reads at node +0x18/+0x1C in the UI timer walk
`func_005BE4C0`, "unresolved indirect call -> 0x74746366" ('ttcf', the
TrueType-collection header of japanese.ttf), then the walk's unbounded node
stack (this+0x3A4, count +0x428) filled memory with 0x19062FA0 up through the
vblank OPD 0x01430BD0. Other boots stalled before the first movie. Boots with
a seeded hdd1 passed.

Cause: on an empty hdd1 the game copies its fonts from
`/dev_bdvd/PS3_GAME/USRDIR/ui/fonts/` (`func_004FE688`: cellFsStat hdd1,
open O_CREAT + close, stat the disc file (at most 3 MB), alloc(size)
through `func_005F785C`, cellFsRead, open hdd1 O_WRONLY, cellFsWrite, free).
For the 2.1 MB japanese.ttf the game's heap returns NULL. `ppu_fs`'s
cellFsRead then fread() 2,197,144 bytes to guest address 0, over the ELF's
first segment from 0x10000 (read-only data: the timer walk's 0.0f at
0x67960, jump tables, vtables). Nothing in our code touched the font after
that; the damage was the read itself. Whether a boot died or stalled depended
on which constant got read next.

Fix: lv2 answers CELL_EFAULT (0x8001000D) and moves nothing for a buffer in
unmapped memory. `ppu_fs.cpp` (`fs_bad_buf`: buffer below 0x10000 or past
4 GB, length > 0) now does that in cellFsRead, cellFsWrite and
cellFsAioRead (the completion callback gets the error), and
`runtime/syscalls/sys_fs.c` in sys_fs_read/write. The first 16 refusals are
logged as `[fs] read fd=.. buf=0x00000000 .. -> CELL_EFAULT (... by <caller>)`.
The game handles the failed copy: it calls `func_00ABCD30` and opens
japanese.ttf from the disc path (`[cellFont] OpenFontFile(...
'/dev_bdvd/.../japanese.ttf')`). The hdd1 copy stays 0 bytes, and the game
retries it each time it loads the font set, harmlessly. The three nimbus fonts
(83-229 KB) copy normally.

Not answered: whether a PS3 heap would also fail the 2.1 MB alloc there. The
buffers that did get allocated sit at 0x1B79xxxx, near the top of the
0xAB00000 block at 0x11000000. Either way the console's behaviour for the
NULL buffer is the EFAULT.

Tools that found it: `FS_CALLER=1` now also logs open flags, cellFsStat
results and every cellFsWrite with its buffer, and the `[fs] read` line
prints the buffer address.

Test: `work/tests/first_boot_fresh.sh` (BOOTS=<n>) boots with an empty hdd1
and save. It taps CROSS until `SAVE complete` and fails on any null call,
unresolved indirect call, bctr to NULL, unregistered OPD or null read in
0x5BE4C0, and on a missing/short nimbus font in hdd1. The other tests still
seed hdd1 from ROOT/hdd1 (`FRESH_HDD1=1` starts them empty).

Measured (copied binary, host load ~40): before, 8 of the 10 fresh first boots
looked at failed (6 corruptions, 2 pre-movie stalls). After:
first_boot_fresh.sh 3 of 3 and 3 of 3, main_menu.sh 2 of 2 seeded and 2 of 2
with `FRESH_HDD1=1`, no null call or unresolved indirect call in any log.

### The /dev_hdd1 UI cache, cellFsRename, and the test seed

The game keeps a UI cache in /dev_hdd1. Each entry is written as "chimp" and
renamed to a hash name (cellFsOpen of the hash first, which misses), 1558
entries and about 1.2 GB in all: the same files RPCS3 keeps in
dev_hdd1/caches/BCES00484_BCES00484439898.

cellFsRename was never registered by ppu_fs. It fell through to
libs/filesystem/cellFs.c, whose mount table has no /dev_hdd1 (and ignores
PS3_HDD1_ROOT), so every rename failed with CELL_ENOENT. No entry ever
persisted. Every boot rebuilt the whole cache (minutes of I/O beside the
menus) and rewrote "chimp" in place in whatever hdd1 the run had. ppu_fs now
implements cellFsRename with the cellFsOpen mapping (`[fs] rename ...
failed` on an error). A first boot now leaves the 1558 files, and later boots
read them. cellFsUnlink and cellFsRmdir, CELL_OK stubs until then, now
delete through the same mapping (a title that deletes a stale entry must
not read it back). Their errors map from errno: ENOENT, EISDIR (unlink of a
directory), ENOTEMPTY, ENOTDIR, EACCES, else EIO. Failures are logged as
`[fs] unlink|rmdir ... failed`, and successes too with FS_CALLER=1.

Measured on a first boot with an empty hdd1 (the seed boot, build-vk 11:4x,
CPUs 0-7,16-23): the 1558 cache files, 1.2 GB, are written over 81 s while
the title and menus run. A boot on a filled hdd1 writes none: 0 "chimp"
opens in main_menu, save_options and first_race on the seed. Before the
rename fix every boot paid those 81 s or more of background I/O. A long
save_options boot once spent its whole 50 minutes rebuilding.

Tests take /dev_hdd1 from a seed, work/runs/hdd1_seed (ms3patch.hdd1_seed,
seed_hdd1):
- It is built by one clean boot with an empty hdd1. Done means the fonts are
  there, at least 1558 cache files, no "chimp" left, and 60 s without a
  change. Its japanese.ttf may be 0 bytes (see "First-boot hdd1
  corruption").
- It is then renamed into place and made read-only. Creation holds a lock,
  so concurrent tests wait and reuse it.
- Each run gets it hard-linked into its own PS3_HDD1_ROOT: writable
  directories, read-only files.
- Never ROOT/hdd1: play.sh and every run without its own PS3_HDD1_ROOT
  rewrite it in place, and a copy taken at 11:11 on 2026-11-07 differed from
  it a minute later.
reach_entry, first_frame, first_draw and boot_past_fios now set their own
PS3_SAVEDATA_ROOT, PS3_GAMEDATA_ROOT and PS3_HDD1_ROOT too.

Every python-driven test also has a hard time limit (ms3patch.hard_limit,
HARD_LIMIT_S overrides). When it fires it kills every process the test
started and fails, instead of hanging a suite. That happened once:
save_options waited 51 minutes on a game that had stopped presenting frames
on the OPTIONS screen. save_options also fails after 120 s with no new frame,
writing gdb stacks to boot*/hang.bt first.

### The Prologue race load (was the next blocker)

Fixed in milestone 4; see "Race" below.

Side note: the game opens `/dev_hdd1/<hash>` cache files directly, and
`ppu_fs` maps `/dev_hdd1/` onto the disc root. They are only read and miss,
so nothing is written, but `/dev_hdd1` should get a host directory of its own
before anything writes there.

## Race

Milestone 4: from "LOADING" into the Prologue race ("Mash's Prologue", a
first boot's automatic first event) and through it. `work/tests/first_race.sh`
drives it (see the end of this section).

### The Havok SPU tasks (the LOADING hang)

Havok's physics runs as SPURS tasks on the `HavokSpursTaskset` Taskset2
(maxContention 5): the PPU (hkSpuUtil, fn 0x8C06FC) creates five
`hkp_integrate` tasks (image 35) per step; each integrate task, when the
next job in the queue belongs to another ELF, creates a task for that ELF
**from the SPU** (SPU libspurs cellSpursCreateTask: hkp_collide, image 33;
hkp_collision_query, image 34; hkp_raycast, image 36) and exits; those in
turn create integrate tasks again. The PPU and the tasks hand work over
through lock-line semaphores (16-byte objects at 0x436F9040.., word = port
byte | waiters byte | count halfword; the PPU half is fn 0x8C20D8 / 0x8C219C,
the SPU half LS 0x4F00 / 0x4DC0 in hkp_integrate), released to the PPU with
`sys_spu_thread_send_event` on the SPURS ports cellSpursAttachLv2EventQueue
bound.

The reported symptom -- one integrate task in `rdch SPU_RdEventStat`, mask
0x400, on line 0x436F9080 -- was not a lost lock-line event: that line holds
the step's semaphores, all at count 0, and the task was correctly waiting for
work that never came. Nothing was wrong with the LR event delivery (the
registry, PPU stwcx./plain stores, PUTLLC/PUTLLUC and DMA PUT all notify).
The chain that should have produced the work was broken in six places:

1. **SPU-side task creation refused (ERROR_PERM).** SPU libspurs'
   cellSpursCreateTask GETLLARs the taskset's 0x1880 line and returns
   0x80410909 unless `size` (+0x1890) >= 0x2900 **and** one of the two event
   flag ids at +0x1898/+0x189C is non-zero (hkp_integrate LS 0x12010..0x12038).
   Our cellSpursCreateTaskset2 left both ids 0, so every create failed and the
   tasks looped TryJoin over 128 ids and retried, forever. CreateTaskset2 now
   creates two real lv2 event flags (`sys_event_flag_create_direct`, new in
   runtime/syscalls/sys_event.c) and stores their ids; DestroyTaskset2 frees
   them. Test: `libs/spurs/tests/test_spurs_taskset2.c`.
2. **SPU-created tasks never started.** The HLE only started tasks it created
   on the PPU, or ready tasks when a sibling exited. A committed SPU atomic on
   a tracked taskset's header line now starts every enabled, ready/pending,
   not-running task (`spu_taskset_header_committed`, runtime/spu/spu_workload.c),
   as the policy module would. Not limited to the taskset's max contention by
   default (`SPU_TASKSET_CONTENTION=1` turns the limit on; `cellSpursCreateTaskset`
   records it): PPU-created tasks start at once whatever the contention, and
   holding back an SPU-created one deadlocked a later step -- the next step's
   five PPU-created integrate tasks sat on a semaphore that the held-back
   collide task would have released.
3. **A task ran under another task's id.** `spu_workload_dispatch_async` read
   the taskset/task id from the single-slot globals the PPU's CreateTask sets;
   a start from another thread in between ran collide task 0 as task 3, with
   task 3's TaskInfo, and left task 0's bits set. The id is now passed down
   (`spu_workload_dispatch_async_task`).
4. **sys_event_flag_set_bit from SPURS dropped.** `spu_deliver_user_event`
   (runtime/syscalls/lv2_register.c) dropped every interrupt-mailbox code >=
   128 from an SPU without an lv2 group. 128/192 (event flag set bit,
   flag id in the out mailbox) route by flag id, so they are handled for SPURS
   too. `SPU_EVT_TRACE=1` logs every SPU user event.
5. **Second code segments were never lifted.** The Havok task ELFs have a
   second PT_LOAD linked high in local store (hkp_collide: 0x9B0 bytes at LS
   0x3BA80; integrate 0x32C00, collision_query 0x38F00, raycast 0x3B900,
   hka_sample_anim 0x3C380). `tools/spu_lifter.py --auto-functions` lifted only
   the first; calls into the rest went to the interpreter. It now detects and
   lifts every executable segment, seeding each with the branch targets the
   other segments reach (and the `--extra-funcs` that fall in it).
6. **Interpreted code returning into lifted code ran the caller twice.** For a
   target in high LS (>= 0x15000) `spu_indirect_branch` ran the interpreter
   with no stop address and returned with pc wherever it stopped: the
   interpreter had meanwhile run on through the lifted caller's own code, and
   the lifted frame then ran it again (collide left its job loop early and the
   step's last jobs were never done). It now stops at the link register the
   branch left (a call's return point; for a return landing in interpreted
   code, the waiting drain's), and dispatches a lifted entry it stops at, as
   the microstep hand-off already did.

With those the load completes: r01_60.avi, the intro cutscene (the opening
credits are drawn over it), the 3-2-1-GO countdown, and the race.

Two runtime-discovered lists live in `work/tools/lift_spu.py`
`TASK_EXTRA_ENTRIES`: entries in hkp_collide and hkp_collision_query that are
only reached through function-pointer tables the PPU builds in main memory, and
call-return points interpreted callees return to (from `SPU_MISS_LOG=1` and the
`[spu] img=N branched into unlifted LS` lines). The MultiStream DSP plugins
stay on the interpreter: the core copies them to LS addresses it allocates at
run time (0x37010, 0x37480, 0x37C20, 0x38230, 0x39810, ... seen), from a heap
copy, so `MS_DSP_LSAS` cannot name them.

### Other fixes on the way

- **MFC engine slots leaked.** `spu_run_lifted_job_abi` runs each task on a
  stack context and unregistered it from the lock-line set but never gave its
  MFC engine slot back. With ~100 Havok task runs a second the 128-slot table
  was full within seconds; every later context then shared the one fallback
  engine (tag state mixed across SPUs) and took the claim lock on every
  channel access. Released now (and in the interpreted task runner). Test:
  `runtime/spu/tests/test_spu_mfc_slots.c` (now builds with gcc; build line in
  the file) runs 384 task runs at fresh stack addresses and checks no slot is
  left behind.
- **Lost wakeup in the channel wait.** `spu_ch_wake` broadcast without the
  waiter's lock, so a producer firing between the waiter's predicate check
  and its sleep woke nobody and the SPU slept the 10 ms re-poll. It takes the
  lock first, and the waiter's check under that lock no longer runs the
  reservation poll (`spu_ch_ready_nopoll`): the poll spins on a lock-line
  stripe's seqlock, and a notifier holding that stripe now takes the waiter's
  lock -- a deadlock (first try froze the race load). `SPU_CHWAIT_STATS=1` counts waits ended by a wake vs the poll
  (about 1.4% by the poll afterwards: a raise with no wake, e.g.
  `spu_resv_lost_poll`'s own).
- **SPURS event flags had no event port.** `cellSpursEventFlagAttachLv2EventQueue`
  was a stub, so an SPU-to-PPU flag's spuPort (+0x0C) named no port. The SPU
  side of cellSpursEventFlagSet sends an event there when a PPU thread waits
  and reads the reply from its inbound mailbox. Attach now creates an lv2
  queue on a free dynamic SPURS port and records port and queue in the flag
  (RPCS3's event_flag_attach_lv2_event_queue is the reference for the
  fields); the delivery path answers CELL_OK for a flag port and wakes the
  flag's PPU waiters (our PPU wait polls the flag word, so nothing is
  queued). Dynamic ports are kept unique across SPURS instances, since the
  port table is global. A flag `_cellSpursEventFlagInitialize` creates with
  no taskset is an IWL flag (isIwl = 1, addr = the instance). Both of the
  game's SPU-to-PPU flags (zlib 0x452F8980, MultiStream 0x0153E700) attach.
  Test: `libs/spurs/tests/test_spurs_taskset2.c`.
- **send_event on an unconnected SPURS port never answered.** MultiStream's
  mixer (image 37) sends `sys_spu_thread_send_event` on SPU port 1, which
  nothing attaches; `spu_deliver_user_event` dropped it without writing the
  result the SPU then reads from its inbound mailbox, so the task blocked in
  `rdch SPU_RdInMbox` for good (the race, and later the whole game, froze a
  few seconds after GO). It now answers CELL_ENOTCONN, as lv2 does.
- **Taskset2 task ids and slot states** (the "physics stopped after GO"
  blocker: SPU create halts, bogus-EA DMAs, then a step that never ends).
  Read from SPU libspurs in hkp_integrate (create LS 0x11F28, TryJoin
  LS 0x127E8; hkp_collide has the same code): the SPU create claims the first
  *clear* `enabled` bit with GETLLAR/PUTLLC on the header line (++u8 +0x71),
  writes TaskInfo, sets pending_ready, and only then GETLLARs
  task_exit_code[id] (Taskset2 +0x1980 + 16*id) and halts (`heqi`, LS 0x126D8)
  unless byte 15 is 0x01 or 0x12; it ORs in 0x10. TryJoin: byte 14 != 0 ->
  ESRCH; low nibble 2 -> exit code = word +8, byte 15 = 0x04, OK; 0x11 ->
  EAGAIN; nibble 3 -> 0x04 and 0x80410920; else ESRCH; on OK it clears the
  id's enabled/running/signalled bits and --u8 +0x71. So an id is owned from
  create to *join*: `enabled` stays set while the task is exited-but-unjoined.
  States: 0x01 free, 0x11 live, 0x12 exited, 0x04 joined-by-SPU.
  The HLE broke this three ways, and each produced the symptoms:
  1. our task exit cleared `enabled`, so creates reused unjoined ids, and a
     later join of the old task freed the new one's id (two tasks per id);
  2. the PPU create's free-id search was a plain scan, racing the SPU's
     PUTLLC claim: both took the same id, and the SPU then found the 0x11 the
     PPU had written and halted;
  3. the SPU create lets the new task start (pending bit) before it marks
     the record, and a 0.5 ms collide task could exit first: 0x01 -> 0x02,
     which the creator's check halts on.
  Now (libs/spurs/spurs_taskset.h "Task id ownership..."): ids are claimed and
  freed by one locked update of the header line (`spurs_taskset_claim_id`,
  `spurs_taskset_free_id`, with the lock-line notify); a Taskset2 exit writes
  0x12 and keeps the id; PPU Join/TryJoinTask2 work on the record state
  (0x12 -> 0x01, then free the id), so they also reap the tasks the SPUs
  created; an SPU TryJoin's 0x04 is turned into 0x01 in the commit hook
  (`spu_taskset_line_committed`), before the joiner frees the id. The
  record helpers write guest memory raw: `vm_write32` takes the lock-line
  stripe itself, and called under it deadlocked the first try (the whole
  physics stopped on the first exit). The name buffer pointer is +0x18A8,
  where the SPU side looks for it. No halt and no rejected DMA in any run
  since. Test: `libs/spurs/tests/test_spurs_taskset2.c` (also races
  simulated SPU creates against PPU creates/joins on 4 threads).
- **Every task ran on "SPU 0".** The SpursTasksetContext the runtime builds
  for a task said spuNum 0 for all of them, and SPURS code indexes per-SPU
  areas by it, so concurrent tasks shared one. After a few thousand raycast
  runs one DMA'd from EA 0xFFFFFFFF (`[mfc] REJECTED malformed transfer`) and
  the physics step never finished. Each running task now gets the lowest
  number no other running task of its taskset holds, modulo the instance's
  SPU count (`spu_num_claim`, spu_workload.c). (A first, process-wide
  numbering went past the SPU count -- the parked MLAA tasks held the low
  numbers -- and Havok's PPU side then called through a NULL entry of its
  per-SPU table.)
- **HDR colour targets on Vulkan.** The engine path refused every colour
  target but RGBA8 ("colour target format 10/12 ... not supported yet").
  `rsx_vulkan_backend.c` now creates float targets (FP16 RGBA = format 10, and
  any format the device can render to and sample), keeps a render pass per
  (format, count), builds each pipeline for its target format and disables
  blending where the format cannot blend.
- `spu_workload` RETURNED lines now say how the task ended (halted, status,
  stop, pc) and how long it ran.

### What the race looks like now

The boot-to-race path: health warning, movies (real frames since the FFmpeg
decoder landed), title, SET SCREEN SIZE, Choose A Control Method (the
default accelerates on R2), the first autosave, r01_60.avi, "LOADING" (the
Havok chain above), the intro cutscene with the opening credits drawn over
it, 3-2-1-GO, and the race with its HUD (the boost/heat gauge, bottom right).
On this machine, shared with other runs, that was 25-35 minutes of wall
time from boot to GO; it is about 8 minutes now (see Speed below).

Rendering: the 3D scene is on screen under the credits and the HUD since the
fixes in "Race rendering" below (it was black: the composite is two NV3089
copies, which the draw engine did not see).

Speed: the intro and race ran at 2-9 fps because the Havok tasks waited
for a CPU, not because they had much to do. Each run's thread CPU time
beside its wall time (the RETURNED line now prints both) said collide
153 ms wall / 16 ms CPU, integrate 56 / 6; `/proc/<pid>/task/<tid>/schedstat`
put the rest in the run queue (a collide thread: 28 ms run, 180 ms
waiting). The cause was `PS3_HOT_PIN=1`, inherited from inFAMOUS 2: it pins
the main PPU thread, rsx, rsx-record and the SPURS kernel SPUs to cores of
their own and confines every other thread -- here the short-lived SPURS
task threads Havok runs on, and the PPU threads polling for them -- to the
remaining CPUs, which the rest of a busy host shares too. With the pin off
(work/port/main.cpp now defaults it to 0), in the same LOADING phase:

| | collide (33) | integrate (35) | collision query (34) | frames |
|---|---|---|---|---|
| pin on  | 69-153 ms wall, 14-16 ms CPU | 29-56 / 6 | 7-36 / 2-3 | 2-9 fps |
| pin off | 2.3 / 1.8 | 1.8 / 1.4 | 1.3 / 0.9 | ~20 fps in the race, 30 in the load |

(A/B on one binary: pin on + yielding polls was as slow as before; pin off
with the usual PAUSE spin as fast as pin off with yielding, so
`PS3_USLEEP_YIELD=1`, a sched_yield spin in sys_timer_usleep, stays an
option, off.) Boot to GO is now about 8 minutes, most of it the movies.

Smaller items done at the same time: the task fingerprint (FNV over a
150 KB ELF, twice per task start) is memoised by (pointer, size)
(`spu_task_fingerprint`); three MultiStream DSP plugins are lifted (see
"Open questions" above) -- before, the interpreter running them was about
45% of the process's samples; collision_query's second-segment table
targets (0x3ACB8..0x3AF70) and MultiStream's 0x21D20 are lifted entries now.
RSS grows to ~11 GB over 30 minutes (not investigated).

### How the Prologue race ends (and what stopped it after GO)

Seen in frame dumps with the UI drawn (runs of 2026-11-04): GO; about six
minutes after the first save the game stops for a button prompt, CROSS,
then shortly after TRIANGLE (a glyph in a dark ring at the top centre; the
physics is paused until it is pressed -- a run with the fixes above but
only R2 held "stopped" here: no Havok task for 120 s); then the race,
with two or three short in-race cutscenes that hide the HUD; then
"Trophy NN 1/3 33%" progress pop-ups, the HUD goes, and an end screen
(the title logo over the scene; with the old renderer just black) with
"(X) CONTINUE" bottom right waits for CROSS. CROSS there is followed at once by
the progress autosave (three `SAVE complete` lines). Boot to that save:
about 20 minutes, the race itself about 10.

The earlier "every Havok task has exited and the main thread waits in fn
0x8C20D8 on the semaphore at 0x436F90A0" stalls came with the Taskset2 halts
and bogus-EA DMAs (fixed above); none of those has been seen since, in six
race runs (three to the end).

### Next blocker

Milestone 4's race runs end to end; what is left on this path is the
picture (libs/video, worked on separately: the 3D scene was black, then
blown-out white while that work was in progress, and is starting to show).
first_race.sh does not depend on it: when a frame is too bright to read
CONTINUE, it presses CROSS once the HUD has been gone for 20 dumps.
Also open: RSS growth, the MultiStream task spinning in its lifted code
through indirect-branch dispatch (~25% of one core in spu_lookup /
spu_drain_call from its LS 0x65B0), and the i3dl2 lift being evicted by the
core's 48-byte writes at its base.

### first_race.sh

`work/tests/first_race.sh` (run_all.sh runs it only with `SLOW=1`): one first
boot on a fresh save root, CROSS every 2 s until the first `[cellSaveData]
SAVE complete`, CROSS for another 60 s (the movie), then `R2=255` held.
The Prologue stops twice early in the race for a button prompt -- a glyph in
a dark ring at the top centre, CROSS (~6 min after the save) then TRIANGLE
-- and waits there with the physics paused until it is pressed; the
controller looks for one in each frame dump (every 60 frames; the glyph's
colour inside the ring) and taps that button. The race ends on CONTINUE,
which it answers with CROSS (seen by the text, or by the HUD gauge being
gone for 20 dumps). PASS when the game autosaves a second time,
with the physics still running and no unresolved NID; it saves the last
frame as `work/runs/first_race.png` and one from three minutes before the
end as `work/runs/first_race_inrace.png`. It fails after `LIMIT_S` (3600 s)
or when no Havok task has returned for 120 s outside a prompt. `OUT=<dir>`
moves its run directory (default `work/runs/first_race`).

## Race rendering

The race intro (the opening credits over a fly-by, from ~1-10 minutes after
boot depending on the load) draws the whole 3D frame, so it is the place to
look; GO is not needed. `work/tests/race_intro.sh` gets there on a fresh save
and checks the presented frame (SLOW=1 in run_all.sh).

### The frame

One world frame, mapped with `PS3RECOMP_ENG_DRAWTRACE` (below): shadow map
0x0C250000 (1024x3072) -> depth/G-buffer 0x00AC0000, 0x01AB0000 (1280x1080
targets, the scene in their top-left corner at a dynamic resolution: 1240x776,
1208x664, 1116x612 ... seen) -> lighting into FP16 0x02000000 -> composite
into 0x01010000 / 0x01560000 (MRT) -> DOF, motion blur, luminance chain
(FP16 0x04130000 320x180 down to 0x003AEB00 10x5) -> bloom (bright pass and
flares into FP16 0x041A0800, blurs 0x0422D200 / 0x04211000) -> tonemap into
0x02AA0000 -> **NV3089** copy to main memory (IO 0x600000, EA 0x41100000) ->
the SPUs' EDGE MLAA in place -> **NV3089** copy into the display buffer
(0x003B0000 / 0x00734000) -> credits/HUD drawn over it -> flip. No draw ever
samples the tonemapped image; earlier notes missed the copies because
`[NV3089]` lines only print with `NV3089_DBG`, on stdout.

### What was wrong

1. **NV3089 copies into a colour target never reached it**
   (`rsx_draw_engine_guest_wrote`, new; cellGcmSys.c `nv3089_blit`). The copy
   writes guest bytes; the engine's display-buffer target is a GPU image that
   only draws change, so the HUD went over black. After a copy the newest
   linear 32-bit target over the destination takes those rows
   (backend `color_target_upload`, Vulkan: a buffer-to-image copy).
2. **The write-back before the copy read the zeta.** The tonemap pass points
   its zeta at its own colour target (0x02AA0000, depth off); the zeta, bound
   later, won the "newest target" test and its cleared 1.0 (0xFFFFFF) went
   into the guest bytes -- a white frame. Zetas now compete by their last
   depth/stencil write or clear (`wstamp`), not by their last bind.
3. **2D-engine methods landed in the 3D register file.** The engine strips
   the subchannel, so NV3089 (subchannel 6/7) and NV3062 (2/3) state at
   0x0300..0x040C overwrote NV4097's alpha test, blend enable/factors, ...
   (an NV3089 SET_OUT_SIZE became the blend source factor). libgcm does not
   resend state it thinks unchanged. Only subchannels 0, 1 (unless
   GCM_SUBCH1_2D) and the driver methods >= 0xE900 feed the engine now.
4. **A geometry-cache entry without geometry was a hit.** `eng_geo_drop_data`
   (a dirty streak) keeps the key with no ranges and no vertices;
   `eng_geo_valid` found zero ranges clean, and after the 64-frame skip the
   draw "hit" with 0 vertices and was dropped as `fetch` -- every frame. The
   bloom bright pass (a quad the game rewrites at local 0 each frame) never
   drew, the flares accumulated in 0x041A0800 (values in the hundreds) and
   the frame went white. A hit now needs vertices.
5. **Half registers alias the full ones** (rsx_fp_decompiler.c,
   `rsx_fp_alias_pairs` / `rsx_fp_alias_sync`). H(2n) / H(2n+1) are the fp16
   halves of R(n) (R(n).x = H(2n).x low, H(2n).y high). The decompiler kept
   r[] and h[] apart; the velocity pass writes H0.xy and stores R0 through
   UP4UB, so the motion-blur buffer held garbage and the frame smeared into
   vertical streaks. Registers a program uses both ways are now synced after
   every write (no change for programs that use one file). Test:
   libs/video/tests/test_fp_decompiler.c `alias_*`.
6. **Y16_X16_FLOAT over an A8R8G8B8 target** (the velocity buffer
   0x01AB0000): the host keeps the target as unorm bytes; such a unit now gets
   `RSX_FP_TEXOP_RAW8_HALF2` and an identity view, and the program rebuilds
   the two big-endian halves (first = A:R, second = G:B) from the bytes.
7. **A texture smaller than the target at its address** is the target's
   top-left corner (the dynamic-resolution frame in a 1280x1080 target,
   sampled as 1116x612): it is bound as a copy of that rectangle
   (backend `color_snapshot_rect`), not as the whole target stretched over
   [0,1]. Re-copied only when the target was drawn since (its stamp).
8. **Alpha reference for float targets** (`rsx_fp_alpha_ref`): tested the
   surface formats 14..16 (X8B8G8R8/A8B8G8R8) instead of 0x0B..0x0D
   (FP16/FP32/X32), so an FP16 target's half reference was read as a byte
   (RPCS3 rsx_decode alpha_ref is the reference for the rule).

FP16 (rsx_be_format 10) and FP32 RGBA (12) colour targets both work on this
device (the 128x1 FP32 targets in main memory included); the race log has no
"not supported" lines left. The one Vulkan error, `vkAllocateMemory(buffer)
failed (-2)`, is the optional 512 MB resident-geometry buffer and falls back.

The stale bands at the left/right edge and the washed-out, blurry picture
are fixed too (next section).

### The post chain on the GPU (2026-11-06)

The copies around the MLAA, as they come in the race (`NV3089_GATE`, or
`PS3RECOMP_ENG_NV3089_LOG=<n>` for what the engine is handed): the tonemap
draws the picture at 20,30 in 0x02AA0000 (up to 1240x1020 at the top of the
dynamic resolution); two unscaled strips (1004 + 236 wide) copy it to the MLAA
buffer in main memory (IO 0x60C000); two scaled copies of a 4x4 texel at
local 0x00382B80 fill the buffer around it; the PPU signals the five MLAA
tasks; and **early in the next frame**, after the composite and before the
next tonemap, the RSX waits on label 0x480 and copies the buffer scaled
(bilinear) into the display buffer at 20,20, 1240x680. Also every frame,
target to texture: 0x01010000 halved into 0x02FF0000 (pitch 2560, sampled by
the motion blur) and a 256x256 of it into 0x0CE50000.

- **Native MLAA** (`PS3_NATIVE_MLAA`, default 1; 0 the SPU path; 2 also
  passes the 0x480 acquire). work/port/native_passes.c describes it to the
  engine (`rsx_draw_engine_set_native_mlaa`: the source target, the label).
  The engine leaves the strips and the fills out of guest memory and does the
  display copy as its own MLAA over the target plus a blit into the display
  buffer's target (`surface_blit` with `RSX_BE_BLIT_AA`). Our MLAA, three
  passes in rsx_vulkan_backend.c: luma edges with local contrast adaptation;
  per edge pixel the run of edges each way (up to 32) and the crossing edges
  at its ends give the silhouette line, whose coverage over the pixel is
  computed directly; a blend with the four neighbours.
  `PS3RECOMP_MLAA_THRESHOLD` (0.1). Safety: a buffer copy goes the guest's way
  until three in a row were seen read back by a display copy; one no display
  copy reads, strips that do not line up, or a draw that samples the buffer
  write the target's bytes into the buffer (`eng_mlaa_materialize`) and go
  back to the guest's way for 8 frames. Only on world frames (>1000 guest
  draws): the boot screens gain nothing from it. **Open, not from this
  change**: boots hang now and then at ~frame 2100-2200 (the loading spinner)
  in the MLAA tasks' barrier -- the PPU in 0x857EF8 on the completion word,
  four tasks in the barrier wait (LS 0xA910 from 0x32FC), the fifth still in
  the signal wait at its start (0xA508 from 0x31AC): its start signal was
  lost. Seen with native MLAA on and off and with the binary from before it:
  eight boots side by side at load ~25-40, the old binary hung in 2 of 4, the
  new one in 2 of 4. The
  acquire is honoured by default: it also keeps the PPU from signalling tasks
  still running, and passing it (mode 2) measured no faster. The skipped rows
  still break SPU reservations on their lines (`gcm_row_touch`).
- **Target to target copies** on the GPU (`rsx_draw_engine_nv3089`,
  `PS3RECOMP_ENG_GPU_BLIT=0` off): an A8R8G8B8 copy whose source is a colour
  target and whose destination is one in local memory is a blit; a
  destination no target covers gets one made at the copy's pitch
  (`eng_blit_new_target`; `PS3RECOMP_ENG_GPU_BLIT=2` keeps those on the CPU),
  which the texture units then bind. With both, the race has no write-back of
  a target and no GPU wait left but the present.
- The CPU copies that remain: a scaled one goes a row at a time (source
  columns once, gathered on the host, `gcm_row_write`) instead of
  vm_read32/vm_write32 a pixel; the write-back before an A8R8G8B8 copy covers
  the rows it reads, not the whole source surface.
- **ZPASS reports**: the race asks for more than a thousand a frame (flares,
  coronas). The report table held 64 a batch and a full one cost a
  submit-and-wait: ~18 a frame, ~9 ms. It holds 4096 now, the query pool
  16384.
- **The washed-out picture**: the small FP16 luminance targets in local
  memory (40x22, 20x11, 10x5) and the 128x1 FP32 ones in main memory never
  reached guest memory -- the write-back was RGBA8 only -- and the CPU
  computes the exposure (and more) from them. Small float targets now go back
  every frame like the RGBA8 ones (`eng_wb_small`, deferred readback,
  big-endian components, R first).
- **The blur**: the velocity pass reads the depth at 0x00AC0000 through
  A8R8G8B8, and a colour target sits at the same address (the depth pass binds
  one there with its colour mask off), so the texture bound was that black
  target: depth 0, a velocity of up to 0.17 of the screen width that depended
  on x alone, and the motion blur smeared everything. A texture at an address
  with a zeta written after the colour target's last colour write
  (`cwstamp`) now reads the zeta (`PS3RECOMP_ENG_ZWINS=0`: as before). And a
  zeta sampled at a smaller size than its extent (760x416 of 1280x1080) is
  copied at that size, its top-left corner, as colour targets already were.
- **The stale bands**: after the CPU's display copy the engine uploaded
  whole rows of the display buffer from guest memory, whose margins no one
  wrote (the GPU had cleared them); only the copied columns go up now
  (`color_target_upload_rect`). With native MLAA the copy never touches guest
  memory anyway.

Measured (both binaries side by side, drive.py, load average 28-40 from other
work on the host): race 11.7-15.7 fps before, 14-20 after with the player's
settings (`PS3RECOMP_BE_THREAD=1` etc.); vk-stats 200-340 submit-and-waits
a second before, ~13 after (the frame dump's present only). The dynamic
resolution goes to its top (1240x1020 source) once the RSX keeps up.

The big black shapes under the cars and on the road near the camera are
fixed: see "Round 2" below (an AO-volume pass read its own target through
guest memory).

### Upstream v0.18 engine features, ported (2026-11-06)

- **Scissored clears** and **user clip planes**: see "Round 2" below.
- Already here before: polygon offset (#562), WPOS in the window convention
  (#589, ours in fp_alpha), depth read as a texture converted on the GPU
  (#499), a depth snapshot of the region the texture names (#507, now for
  colour-format reads too).
- **3D textures**: see "Round 2" below.
- `[vk-stats]` also reports from the present (and presents/s): it used to
  print only from a flush, and with frames in flight and no flushes the
  player's configuration logged nothing.

### Round 2: 3D textures, own-target snapshots, rect clears, user clip (2026-11-07)

All four are on by default (main.cpp); each `=0` turns it off.

- **Own-target snapshots** (`PS3RECOMP_ENG_OWN_RECT`, rsx_draw_engine.c
  `sink_bind_textures`): the fix for the black "extruded shadows". The
  composite is `albedo * (L * (c3 - c2 * (1 - a)) + a * ambientCube(n) * k)`
  with `a` the alpha of the G-buffer 0x01AB0000. After lighting, the
  AO-volume pass (boxes around the cars: 48 vertices, depth GREATER,
  front-face cull, alpha-only write) samples that same G-buffer as a
  1240x1020 texture (the dynamic resolution) while drawing into the
  1280x1080 target. The read-modify-write snapshot only matched an own
  target of exactly the texture's size, so the unit fell through to guest
  memory (stale bytes), `a` became 0, and wherever the sun was shadowed the
  pixel went pure black. Such a unit now binds `color_snapshot_rect` of its
  own target, taken just before the draw. Found with
  `PS3RECOMP_ENG_DRAWTRACE_PX` on a black pixel (G-buffer fine, sun fine,
  then the AO pass set alpha 255 -> 0) and confirmed with
  `PS3RECOMP_ENG_SKIP_VOL=1` (drops every draw that samples a 3D texture).
- **3D textures** (`PS3RECOMP_ENG_TEX3D`): engine `eng_texture_upload`
  walks a volume level by level and slice by slice (swizzled: one 3D Morton
  block per level, `rsx_swizzle_offset_3d`; linear and DXT: consecutive
  slices), with its own cache key and span. Backend hooks
  `texture_create_3d` / `texture_upload_3d`. Vulkan: a VK_IMAGE_TYPE_3D
  image and a 3D view (the unit's crossbar in it), a third descriptor bank
  `rsx_tex3[16]` at t64 (binding 80) and a 1x1x1 dummy. Decompiler:
  `rsx_fp_decompile_buffered_ex3` with `tex_vol_mask`, so those units are
  Texture3D sampled with xyz. The race binds five (32x32x16 A8R8G8B8 at local
  0x002E6900, 16x16x16 B8 AO volumes at 0x04367A00 / 0x04896A00 /
  0x0425EB00, a 512x64x4 DXT5); `PS3RECOMP_ENG_TEX3D_LOG=1` prints each
  slice's byte range (the AO volumes: 254 at the faces, ~9 in the middle).
  As 2D first slices they left a green patch behind the car and no AO.
- **Scissored clears** (`PS3RECOMP_ENG_RECT_CLEAR`, upstream #560; `2`
  colour only, `3` depth/stencil only): a CLEAR_SURFACE under a scissor
  narrower than the surface clip clears only that rectangle
  (`eng_clear_rect`, backend `clear_color_rect` /
  `clear_depth_stencil_rect`). MotorStorm clears at the dynamic resolution
  (1240x1020 of 1280x1080, 620x510 of 640x540, ...). A partial depth clear
  keeps the zeta readable. Vulkan does the fill on the GPU: one row by
  vkCmdUpdateBuffer into the device scratch buffer, doubled by buffer copies,
  copied into the image. The first version filled from the staging ring
  (~6 MB a frame for 1240x1020 depth + stencil), and its flushes slowed the
  second boot's splash past main_menu.sh's 180 s.
  `PS3RECOMP_ENG_CLEARLOG=1` logs every clear's rectangle.
- **User clip planes** (`PS3RECOMP_ENG_USER_CLIP`, upstream #537):
  SET_USER_CLIP_PLANE_CONTROL (0x1478) feeds `rsx_vp_apply_user_clip`
  (SV_ClipDistance from o5/o6.yzw) and the pipeline key; Vulkan enables
  shaderClipDistance only with it.
- **Antialiased surfaces**: the game never sets one (the one-time log
  `[rsx engine] antialiased surface` stays silent through the intro and the
  race start), so there is nothing to do.
- Tools: DRAWTRACE also logs depth bounds, depth range, the whole stencil
  state and every fp constant. DUMP_SURFACES writes `<name>_a.pgm` with each
  target's alpha. `PS3RECOMP_FP_PERSP=0` ignores the fp
  perspective-correction flag (diagnostic). work/runs/rsx2/drive.py drives a
  boot to world frame N and can trigger a DRAWTRACE there.
- Validated with everything on: main_menu 2/2, race_intro, and the race
  start (work/runs/race_shadow_fix/fin_last.png).

### Split-screen (2026-11-07)

Wreckreation > Quick Race is the offline split-screen mode (up to 4 players).
Player n joins with START on port n-1. Each join calls
**cellUserInfoSelectUser_ListType** (the profile picker). That call was
unresolved, its callback never came, and the menu sat on "Waiting". It now
completes with the one user (libs/system/cellUserInfo.c; also
`_SetList`), and player 2 shows as "(1)". Start Race, then CROSS at the
fly-by.

How the RSX draws it (a vp DRAWTRACE of a 4-player race): each view is a
whole pass of its own, in turn. Shadow map 1024x3072 (two cascades), depth,
G-buffer, lighting, composite, post, all in the top-left 620x508 of the
1280x1080 targets. Then one draw puts the view at its place in the shared
frame (viewport translate 330,284 / 950,284 / ..., scissor at 20,30 and
640,30 ...). 2 players: top/bottom, 868x340 each, pillarboxed. 4 players:
quadrants, 620x340. Nothing here needs new code: per-view dynamic resolution
goes through the own-target snapshots and rect clears above. Pictures:
work/runs/splitscreen/2p.png, 4p.png.

Driver: work/runs/rsx2/mp/ -- launch.py (PADS=n virtual file pads,
optional seeded save), nav.py (second boot -> Quick Race), tap.py (presses
and a snapshot of the last frame).

**Performance** (seeded race, 60 s, `PS3RECOMP_VK_HITCH=1` every frame,
averaged by work/runs/rsx2/mp/stats.py; host loaded by another project, so
compare components, not fps). Before: 2P 57-95 ms a frame, 6 submit-and-waits
(12 write-backs of the tonemap target for the SPU MLAA), 16-36 image
allocations, 166 snapshots (6-18 ms). 4P 89 ms, 6 flushes, 18 write-backs.
Three new switches (default off until integration):
- `PS3RECOMP_ENG_MLAA_VIEWS=1`: native MLAA holds one state per view (up to
  4). Each view's buffer copy and display copy are matched by where they sit
  in the buffer. Before this, every view after the first fell back to the
  SPU path with a write-back and a wait. Now there is 1 flush a frame and no
  write-backs.
- `PS3RECOMP_VK_MEMPOOL=1`: image memory (`vk_create_image_levels`) comes
  from power-of-two size-class free lists. Textures are not pooled.
- `PS3RECOMP_VK_SNAPCACHE=1`: `color_snapshot_rect` keeps four copies per
  target, one per size, instead of one remade at every change of view size.
2P with all three: 1 flush, 0 allocations, snapshots 1.6 ms, RSX thread
~50 ms a frame (was ~70). 4P: 85 ms a frame, RSX thread ~70 ms. What is
left is per draw, not per view: `PS3RECOMP_ENG_DRAWSTATS` at 4P (~11900
draws a frame) shows backend draw 28.7 ms, textures 7.7, layout 5.8,
constants+binds 5.2, pipeline 4.6. It also shows 64 MB of vertex constants
staged a frame (5.4 KB a draw, unchanged in only 148 of 11900 draws). That is
the next target, for 1P as much as split-screen.
work/tests/splitscreen.sh (SLOW) is the regression: 4 players join, the race
clock runs, 4 views with a picture.

### Tools

- `PS3RECOMP_ENG_DRAWTRACE=<world frame>,<dir>[,fs|,vp|,<first>,<last>]` with
  `PS3RECOMP_ENG_DRAWTRACE_AT=<file>`: a script creates <file> and the next
  world frame is traced. Per draw now also: viewport/scissor/window, blend,
  alpha, cull, depth state, the first vertices and vertex-constant rows of
  small draws, every clear, every dropped draw and why (`dropped before dN`),
  and float targets' unclamped mean/max. `,vp` logs the state of every draw
  without the per-draw readbacks (seconds, not minutes).
- `PS3RECOMP_ENG_DUMP_SURFACES` / `_PROBE` / the trace read float targets back
  correctly (they wrote 8- and 16-byte pixels into 4-byte buffers) and name
  them `_fp16` / `_fp32`.
- `PS3RECOMP_VK_SKIPLOG=<n>`: the first n draws the Vulkan backend drops, and
  why.
- `NV3089_GATE=<file>`: every blit while <file> exists (stdout; run the game
  under `stdbuf -oL` to see them in time).

### race_intro.sh

`work/tests/race_intro.sh`: fresh save root, CROSS every 2 s until the first
save and for 60 s more, then waits for 200 world frames
(`PS3RECOMP_ENG_WORLDLOG`) and checks the frames dumped after that: PASS when
one has a central-60% luma mean in [12, 235] with a standard deviation of at
least 12 (the black screen and the white one both fail). Saves
`work/runs/race_intro.png`. 3-15 minutes, mostly the race load.

## Every track (all_tracks.sh)

### Booting straight into a race

The game mounts patch archives over ms3.psarc: it looks for
`ms3patchv0102.psarc` and `ms3patchv0101.psarc` in
`/dev_hdd0/game/BCES00484/USRDIR/` (then on the disc). Their files replace
the base archive's by path (PSARC v1.4, zlib, 64 KB blocks, flags 3 =
ignore case: the TOC key is the MD5 of the upper-cased path). The game flow
lives in `/data/gamestate/gamestates.xml`: states (boot, mainmenu,
QuickRacing, festivalracing...), the transitions between them, and the
GameConfigs they read. `work/tests/all_tracks.sh` patches two things in it:
- `boot -> booted` goes to `QuickRacing`, not `mainmenu`;
- the `quickrace` GameConfig gets `ticket="Track NN"`, `World`,
  `Environment` (`sky_<track>`), `Gags` (`track_NN`), `GameMode race.mode`,
  `GUIResource Art/UI_Art_BE.rpk`, `IsRaceConfig`, three AI players and one
  local human, and 1 lap. The ticket alone does not set the world
  (`worlds/.rpk` is opened and the loader dies in its error path,
  func_00573F8C), so World and Environment are given explicitly.

The game then boots into the pre-race flyover (START RACE: CROSS) with no
movies, no menus, and no save needed, in 20-55 s. Each track gets its own
PS3_HDD0_ROOT holding only that archive. The gamestates.xml it patches is
read from the disc's ms3.psarc at run time, so no game data is kept in the
repo.

Dead ends: an argv `.cfg` (`PS3_ARGV=<name>.cfg`) replaces
`Configs/ms3_config.cfg` (func_002CD150 looks it up on hdd0, the disc, then
`configs/` in the psarc). The disc's own race configs (autotest_*,
*_4player, benchmark_*) are stale: they name skies that no longer exist
(sky_downtown_midday) and `.gui` files. An untyped config goes to the
`gostorm` state and runs out of a GUI object pool (pointer pool at
[0x144F118] read past its end, ctor on 0xA via func_006CF6C8). A
`COMMANDLINE_RACE` config goes to the main-menu flow and never leaves the
loading screen.

The tracks are the game's 39 `Track NN` tickets (alltickets.zip in
ms3.psarc): dogpatch v1-4 (Docklands), pier v1-3 (Boardwalk), haight v1-4
(Upper Bohemia), yerbabuena v1-4 (Good Herb), interstate v1-4, union v1-4
(Downtown), market v1-3 (Market Garden), mainline v1-3, transbay v1-4
(Terminus), vertigo v1-3 (Skyline), alcatraz v1-3. The alcatraz worlds on the
disc are 2-16 MB greybox placeholders (the real ones are the ALCATRAZ.edat
DLC the game probes for), so they render as grey geometry: expected. The
prologue tracks are not in the list (first_race.sh covers the Prologue).

### all_tracks.sh

`work/tests/all_tracks.sh [track ...]` (PAR=<n> games at once, LIMIT_S per
track, default 600) taps CROSS every 3 s until the race HUD shows. A track
passes when:
- the yellow POS shows at the bottom left;
- the race clock box (top right) changes over 3 dump pairs (it starts at GO);
- Havok tasks are still returning;
- the log has no null call, unresolved call or NID.
It keeps `<track>.png` and `summary.txt` under `$RUNS/all_tracks/`.

`PLAYERS=2|4` makes 2 or 4 local human players (split-screen), each a
`viewport="i"` Player with its own virtual pad (`PS3_PAD_FILE_<i>`). The HUD
and clock boxes move with the layout: 2 players are top/bottom views,
pillarboxed (POS x 200..330 y 650..705, clock x 990..1075 y 14..40); 4
players are quadrants (bottom-left POS x 25..110 y 650..700, top-right clock
x 1170..1250 y 12..36). 3 players uses the 4-player boxes and is untested.
`KEEP=1` keeps every frame dump. Checked: union_v1 and mainline_v1 reach GO
with 2 players, and mainline_v1 with 4. union_v1 with 4 was still at 00:14
on the race clock when a 300 s limit ran out (a dump every ~6 s at load
~40), so keep the 600 s default for split-screen. (Black geometry seen in
2-player on an older binary is gone with the shadow fix of 44b87d1.)

Measured (fix1 copy, PAR=2, load ~40): 39 of 39 reach GO in two full runs, the clock running
21-56 s after start. About 25 min for the whole list. The pictures still
show the known black shadow shapes and, on dogpatch_v1 and pier_v2, an
overexposed sky band.

### Game modes (MODE=)

The Quick Race menu (ui/fe/offlinewreckreation.xml, QuickRaceSetMode)
offers FESTIVAL, RACE, ELIMINATOR and CHASE. FESTIVAL there is a race with
the track's incidents (Gags), which is what MODE=race already loads.
`MODE=race|eliminator|chase` puts `data/gamemodes/<MODE>.mode` in the
patched quickrace config. Checked on union_v1, mainline_v1 and dogpatch_v2:
eliminator and chase both reach their running state (the eliminator's
"ELIMINATIONS BEGIN IN" countdown, chase's rounds and score), and the clock
box sees their timers run. Time attack is not reachable this way. Booting
into `timeattackracing` (noperkrace config, ticket "Time Attack Infinite -
Track NN"), or QuickRacing with timeattack.mode, dereferences NULL in the
first frame (func_002A79D4, func_00ABCBDC). That controller wants state the
challenge lobby sets up.

Booting through the patch route skips the menus, and with them
`FE/EndRaceScreen`: the frontend sets it when a race is started
(Wreckreation_Race_Complete, Festival_Race_Complete...,
ui/fe/carrier_menus.xml). Without it the HUD stays on YOU FINISHED forever
after the finish, because the HUD's end-of-race Flow has no target. This is
not an emulator bug. Tests that need the end of a race set it, or go through
the menus.

### save_options.sh

`work/tests/save_options.sh` makes two boots on one fresh save root, each
patched to boot straight into OPTIONS (`boot -> booted` goes to the
`options` state).
- Boot 1: DISPLAY, then SUBTITLES. Picture A is the value. RIGHT flips it
  (picture B). Backing out saves the profile (`SAVE complete`).
- Boot 2: the save loads (`new=0`) and the same path gives picture C. It must
  match B and not A.
Navigation is a loop that reads a frame dumped after the previous press and
acts on the screen it shows:
- DISPLAY SETTINGS (a value box on its 4th row, and a short subtitle):
  DOWN until the yellow `< >` sits on SUBTITLES.
- The OPTIONS list (the yellow icon panel on the right): UP x5, DOWN, CROSS.
- Another OPTIONS sub-screen: CIRCLE.
- No OPTIONS title for 4 s (SET SCREEN SIZE): CIRCLE.
A press made while a screen slides in is lost under load. A fixed sequence
once ended in OVERLAY TRANSPARENCY (integ7 SLOW) and once in WIRELESS
CONTROLLER SETTINGS, whose list also has value boxes, hence the subtitle
check. It also fails on any `sceNpTrophy`
error. 2 of 2 PASS (|A-B| 13-14, |C-B| 2, |C-A| 13).
Trophies are kept in `$PS3_GAMEDATA_ROOT/trophies` (sceNpTrophy.c), and by
default that is the player's `gamedata/trophies`. The tests set
PS3_SAVEDATA_ROOT but not PS3_GAMEDATA_ROOT, so a race in a test unlocked
trophies into the player's file (gamedata/trophies/NPWR00899.json changed at
03:19 on 2026-11-07). all_tracks, save_options, festival and
first_boot_fresh now give each run its own PS3_GAMEDATA_ROOT; main_menu.sh
and first_race.sh do not yet.
`work/tests/ms3patch.py` has the shared helpers: read/write PSARC, the stock
gamestates.xml, the AutoPilot HUD, and frame boxes.

### festival.sh (Festival mode)

`work/tests/festival.sh` (SLOW) plays the first Festival event after the
Prologue to its end and checks the progress survives a reboot. It needs a
post-Prologue save: `SAVE_FROM=<root holding BCES00484/>`, otherwise it runs
first_race.sh first.

The game drives the car itself. The only patch (ms3patchv0101.psarc in the
run's PS3_HDD0_ROOT) is the race HUD: GameHUD sets
`CompetitorManager/Competitors[i]/AutoPilot` true for every competitor whose
`Finished` is false (ms3patch.autopilot_hud). AutoPilot is the flag the game
puts on a car after it finishes, and the AI drives it. The boot, the menus
and the Festival flow are untouched: movies, title, MAIN MENU > FESTIVAL >
FestivalPopup "Continue" (which also sets FE/EndRaceScreen to
Festival_Race_Complete), the chapter's motion comic and intro card, the
race, and the results.

The pad policy (from the frame dumps):
- While the race HUD (yellow POS) shows, nothing is pressed.
- On the not-qualified results it presses SQUARE, then CROSS to confirm the
  retry. A car that finishes
  outside the top 5 gets QUIT (CIRCLE) / RETRY RACE (SQUARE) instead of
  CONTINUE, and that screen is told apart by QUIT's pink circle where
  CONTINUE's cross would be (x 930..962, y 640..668).
- Anything else gets CROSS. A blind SQUARE every third tap also retried
  races that had been won ("CONGRATULATIONS! You won the race!", then the
  race again), so it is only pressed on that screen.

Boot 1 ends at the first `SAVE complete` after a race HUD. Boot 2 reboots on
that save and goes the same way to the next race HUD. The event is the track
world read last before the HUD, worked out from the ms3.psarc read offsets
(FS_CALLER=1 logs every cellFsLseek; ms3patch.psarc_ranges maps an offset to
its file). The boot also reads a header of the DLC worlds (alcatraz, market,
two prologues). Boot 2 must continue on a different world than boot 1's
event.

The first event after the Prologue is Upper Bohemia (track_haight_v1), "Day
One - Morning", a buggy in a field of 16, 3 laps. The next is Good Herb
(track_yerbabuena_v1). Measured: race HUD 150 s after boot, event finished
and saved at 458 s (923 s in a run that had to retry), and boot 2 continued
on track_yerbabuena_v1. festival.sh on the final pad policy and earlier
ones: 3 PASS (fest3, fest5, fest8 under work/runs), 1 hang (fest4, below).
Two runs (fest6, fest7) were stopped while the retry handling was being fixed. Under AutoPilot the car
has finished from 1st to below 5th.

Intermittent hang in the race (2 of 6 races so far, both on haight_v1, at
~0:35 and ~1:05; reported to ppu2). The frame dumps and the Havok task
returns stop for good. In one run a "ppu main" thread spins at ~110% CPU, in
the other only an SPU "task0" thread (one of the persistent images 30/31/37,
which never return in any run). The dispatches and returns of images 32-36
balance in both logs. festival.sh starts the game with
PR_SET_PTRACER_ANY (ms3patch.allow_ptrace). When no frame has been dumped
for 120 s it writes every host thread's stack (gdb `thread apply all bt 40`;
lifted functions are func_<addr>, so it is the guest chain too) to
`<boot>/hang.bt` and fails.

## The 0xD0000000 crash (func_00911128 packet lists)

A host SIGSEGV seen in races, about 1 race in 10 in ppu2's hunts: a
16-byte store (stvx) to guest EA exactly 0xD0000000, in func_00911128, on the
main PPU thread.

func_00911128 walks lists of packets:
- The first list is arg r4: a size word at +0, packets from +16. The cursor
  is r19; the end, list + size + 16, is kept at [r1+0xEB8].
- Each packet's byte at +3 is its size in bytes, a multiple of 16. The copy
  loops at 0x9113F8 and 0x911480 move (byte >> 4) quadwords, two per pass
  (`r9 = n-1; ...; r3 -= 2; bne`). They stop only for counts 1..15.
- With a count of 0, r3 runs -2, -4, ... and never reaches 0. That is a
  runaway copy of 32 bytes a pass, upward through the flat guest window,
  until the first page that cannot be written: the guard page under the
  first guest thread stack (VM_STACK_BASE). So the EA is always exactly
  0xD0000000, whatever the list. Everything from the copy's target up to
  there, RSX local memory at 0xC0000000 included, is overwritten first.

A zero header would be just as fatal on a PS3, so the list was read before
whoever builds it had finished, or after a lost update (ppu2's coherence
hunt).

The guard: tools/ppu_native_entries.py NATIVE_BLOCKS puts a check at
loc_00911378, the top of the per-packet loop.
- A cursor at or past the end, or a header with a count of 0, sets r19 to
  the list end and jumps to the function's own end-of-list check
  (loc_00912348). The list is cut at the bad packet.
- port/ppu_native.cpp ppu_pkt_guard logs `[pkt-guard]` with the cursor, the
  end, 64 bytes there and the guest LR back-chain, for the first 16 distinct
  (cursor, end) pairs, then a count at powers of two.
- On by default. PS3_PKT_GUARD=0 turns it off (the crash returns).
  PS3_PKT_GUARD=2 also calls spu_coh_dump() on the first hit.

Not the cause: the 256 MB of VRAM cellGcmGetConfiguration used to report.
A retail PS3 reports 0x0F900000; that is now reported
(gcm_reported_local_size, GCM_LOCAL_SIZE overrides), and the crash happened
with it too.

work/tools/segv/segvdump.so is an LD_PRELOAD fault dumper. Its handler is
installed from a constructor, under the runtime's chained ones, so it only
sees the fatal fault. It writes the address and guest EA, every host
register, 256 bytes from rsp and a func_ backtrace to $SEGVDUMP_FILE.

A different segfault (fios scheduler 4, EA 0xD0076F40, in func_00ABDAB8, the
CRT's printf) was that thread's 64 KB stack overflowing by 0xC0 bytes into
its guard page. It happened in a run booted from the disc's stale
union_v1_4player.cfg, which had already overrun the GUI object pool: not
real play.

## Simulation pacing

As shipped, the game steps its simulation a fixed 1/30 s every frame and
flips every second vblank: on the PS3 a steady 30 fps. Here a race frame
takes 50-120 ms, so the race ran at 0.4-0.6x real time. `PS3_SIM_60` (default
1; `PS3_SIM_60=0` is the game as shipped) makes the step real-time and the
flip interval 1. main.cpp sets the default, work/port/ppu_native.cpp does the
work.

### How the game keeps time

- **Clock source** (the one instance is constructed by 0x5EE2A4, called from
  0x2D6C90; heap 0x450E36B0 in every run so far): +0x00 int64 step (us),
  +0x08 slack, +0x10 refresh Hz, +0x14 float scale, +0x18 vsync interval,
  +0x1C real-time flag, +0x20/+0x21 slack flags. The constructor's defaults
  are 60 Hz, interval 1, real-time. Setters: 0x5EE3B4 Hz, 0x5EE3D8 +0x20,
  0x5EE3FC scale, 0x5EE420 interval, 0x5EE444 +0x21, each ending in 0x5EE178,
  which recomputes the step (interval * 1e6 / Hz * scale: 16666 or 33333); the
  flag is a plain store, 0x4F8F60. The game's setup, 0x2D6F30 (0x2D6E58 the
  same from 0x1E3F28), sets 60 (50 on a 50 Hz display) Hz, flag 0, scale 1,
  interval 2. Other configurations: 0x1E3F28 (real-time, interval 1: the game
  has a real-time mode of its own), 0x1D90C8 (30 Hz, interval 1, fixed; from
  the race update, behind a flag), 0x1F1F58/0x1F37A8 (flag 0, scale), and the
  crash slow motion through the scale (0.35, 0.107 -> steps 11666/3579 us). A
  second, stack-local clock (0x5EE310 from 0x1D5680: 30 Hz, fixed) is
  something else and must stay fixed.
- **Frame timer** (0x5EE620, the present's timer at device+1000): flag 0 adds
  the step, flag 1 adds (now - last) * scale from sys_time_get_system_time.
  The race reads it as dt (int64 us at [[[[0x144FB4C]+0x29C]+0x294]]+0x78),
  -> 0x5BE4C0 -> func_001DA398 clamps it to 0.04 s (float compare 0x1DA384,
  double result 0x1DA388) -> func_00EC8A98 snaps 0.031..0.035 s to exactly
  1/30 (0xEC8A80/88/90) -> the race clock float 0x14FC348 (0x739540 from
  0x415DA0). At 60 fps (16.7 ms) neither the clamp nor the snap engages; at
  30 fps the snap makes dt exactly 1/30; under 25 fps the clamp makes the race
  run at 0.04 * fps (0.65x at 16 fps). PS3_SIM_60 changes both (below).
- **Present** (object [0x144F150], func_006AF3E8 from func_006AFD30): mode
  +0x64/+0x68 = 1 (vsync), interval +0x74 = the clock source's +0x18, stored
  every frame (0x6AF7D4), so the interval follows the clock. The vblank
  handler (OPD 0x01430BD0 -> 0x6B05E4 -> func_006B02BC) ignores its argument,
  adds 1 to +0x6C per call and releases the frame when +0x6C >= the last
  flip's +0x70 + interval.

### What PS3_SIM_60 does

- `nat_clock_ctor` (native hook on 0x5EE2A4) remembers the instance;
  `nat_clock_recompute` (on 0x5EE178) sets its +0x1C to 1 and an interval
  above 1 to 1 before every recompute. Every configuration sequence ends in a
  setter, so the game's flag-0/interval-2 setup and the others all come out
  real-time with interval 1; the scale (slow motion) still applies. Both
  hooks decline: the lifted bodies run on the patched fields. They are in
  tools/ppu_native_entries.py NATIVE_ENTRIES (`PS3RECOMP_NATIVE_PPU=0` turns
  them off with the other natives). Log: `[sim60] game clock source at ...`
  once, `[sim60] clock ... made real-time` for the first 16 fixes (two at
  boot: the setup's flag and interval).
- Frame rate: `PS3_SIM_FPS=60` (default) or `30` (settings.ini
  `[performance] frame_rate`, the F1 menu's Frame rate, live via
  `port_sim_set_fps`). 30 keeps the game's interval 2 (a flip every second
  vblank) with the real-time clock. Measured windowed, 1x, menu (2026-11-09):
  60 gives 45-49 presents/s, which on a 60 Hz screen alternates 1 and 2
  refreshes a frame (the judder players see); 30 gives 30.0 presents/s.
  Until the host holds 60, 30 is the smooth choice. Mailbox is missing on
  COSMIC's Wayland surface (modes: fifo, immediate, fifo-latest-ready), so
  `vsync = 0` falls back to immediate.
- Performance HUD (ui.cpp, after the Pacific Rift port's): F10 cycles
  detailed (fps against the target, avg/worst ms, 1% low, the share of
  frame-to-frame swings over 4 ms, a frame-time graph, render size, draws,
  process CPU), compact (fps only) and off; `fps_counter` 0/1/2. F2-F10 and
  F12 reach the port through `rsx_ui_set_hotkey` (libs/video/rsx_ui.cpp);
  PS3RECOMP_UI_KEYS takes F10.
- `GCM_VBLANK_EACH=1` (set by main.cpp with PS3_SIM_60): the runtime calls the
  vblank handler once per vblank. The RSX thread ticks the 60 Hz vblanks it
  owes in a burst after each drain, and the pump folded them into one pending
  bit, so the handler's count ran at ~2-4x the frame rate (19-49 a second
  measured at 8-16 fps) instead of 60. This is a change to
  ps3recomp/libs/video/cellGcmSys.c, now in work/patches/ps3recomp.patch
  (it was work/patches/pending/vblank_count.diff until the v0.18.0 rebase);
  a binary built without it ignores the variable and keeps the folded count,
  which at interval 1 only delays a flip by up to one drain.
- The dt clamp is raised to 66.7 ms (1/15 s): the clamp constants at
  0x1DA384/0x1DA388 are rewritten once (after checking they hold 0.04), so
  the race keeps real time down to 15 fps. With the game's 40 ms, the port's
  24-31 fps race lost every frame over 40 ms (sim_speed.sh: 0.88x at 25.5
  fps). `PS3_SIM_DT_MAX_MS=<ms>` (20..200) sets another; 40 is the game's.
  Havok steps once a frame with this dt: its task runs per 60 frames are
  ~1600 at 33 ms and at 100 ms steps alike, so there are no fixed substeps,
  and physics takes the frame's dt, jitter included (6 minutes of race at
  100 ms steps showed nothing wrong).
- The 1/30 snap (func_00EC8A98: 0.031..0.035 s -> 1/30) is turned off by
  raising its low bound (double 0xEC8A80) to 1e30: for a real-time clock it is
  a 5% error per frame around 30 fps. `PS3_SIM_SNAP=1` keeps it.

### Measured (load average ~40: three games and a toolkit build at once)

| run | race fps | race clock / wall | dt (us) | vblank handler calls/s |
|---|---|---|---|---|
| PS3_SIM_60=0 | 7-17 | fps/30 (0.23-0.56) | 33333 always | 19-49 |
| PS3_SIM_60=1 | 7-14 | 0.04 * fps (0.26-0.58), 0 windows off of 64 | wall frame time | 60.0 |
| + PS3_SIM_DT_MAX_MS=100 | 7-11 | 0.69-0.96 (median 0.96 where 0.1 * fps >= 1) | wall frame time | 60.0 |

No race frame reached 25 fps under that load, so the full-speed band was
measured only with the 100 ms clamp; with the default clamp it needs frames
of 40 ms or less. The menus run with the interval at 1 too (present +0x74 =
1, the clock's +0x18 = 1, +0x1C = 1, read live) and, with
PS3RECOMP_FPS_LIMIT unset (as in every headless run), went over 30: 30-38 fps
on the first-boot "Choose a Control Method" screen at load average ~20. The
stretch before the intro where the race clock stands still (the r01_60.avi
movie, by where it falls in the boot) ran at 30.7-32. first_race.sh with PS3_SIM_60=1 (the default
now): PASS, race over 841 s after the first save at load ~40 (278081 Havok
task runs; prompts TRIANGLE at 365 s, CROSS at the end); run_all.sh: 5 of 5.

### Things that do not use dt

- The audio stream thread (func_00068380) paces itself on the wall clock:
  each pass sleeps 33333 us minus the time it took (`ori 0x8235` at
  0x6930C), 4 ms when over. Independent of the frame rate.
- The EBOOT holds the 1/30 float (0x3D088889) 31 times, mostly in
  per-function constant pools; the functions found loading one: 0x1E4E8, 0x124138, 0x1E3268, 0x4254C8,
  0x47D950, 0x488378, 0x49CFE0, 0x4A3E28, 0x4D0818, 0x4EBDF0, 0x5337D0,
  0x534BE8, 0x56FBD0, 0x570A28, 0x5BA728, 0x762DE0, 0x7899A0, 0xEC8448 (and
  the snap, 0xEC8A98). None has been read one by one. If something runs at
  the wrong speed at a high frame rate, start there.
- SPU job batches are queued per frame and finish within it, so they follow
  the frame rate, as on the hardware at 60 fps. Havok's task runs kept coming
  at every frame rate measured; whether its step takes the game's dt or a
  fixed one has not been read.

### sim_speed.sh

`work/tests/sim_speed.sh` (SLOW=1 in run_all.sh): runs first_race.sh and
samples the game's memory every 0.5 s (frame counter, vblank counter, race
clock, dt) until the race clock has moved for SAMPLE_S (300) s, then stops
it. In 4 s windows where the clock moved and the game presented: clock/wall
within 0.95..1.05 when clamp * fps >= 1, else within 0.70..1.10 of
clamp * fps, the clamp read from the log's `[sim60] frame dt clamp` line
(40 ms if the binary does not print one); 90% of the windows must pass, no more than half the samples may
have the stock dt 33333, and the handler must run 50..70 times a second.
With PS3_SIM_60=0 it fails ("96% of the samples have the stock fixed dt").

## Toolkit rebase to v0.18.0

ps3recomp moved from v0.13.0 (42f5f8f9) to v0.18.0 (4e8f3ca8, 848 upstream
commits, 542 files). `scripts/setup.sh` and `scripts/make_patch.sh` pin the
new BASE; `work/patches/ps3recomp.patch` is now 199 files, +36.3k/-11.6k
lines (was 211 files, +43.7k/-6.1k against v0.13). The old tree is kept in
the ps3recomp repository as branch `pre-v0.18` (one commit on 42f5f8f9 holding
the old working tree); the rebase is branch `rebase-v0.18`.

How: the old tree committed, cherry-picked onto v0.18 with
`-Xignore-cr-at-eol` (upstream turned the CRLF lines of several runtime/spu
files into LF), 83 files in conflict, resolved area by area. Where both sides
had rebuilt the same subsystem along different lines (draw engine and Vulkan
backend, SPU coherency/context/float helpers, the SPURS kernel and taskset
scheduling, cellSpurs, cellFont), ours was kept whole -- it is what runs this
game -- and upstream's interfaces and the fixes that apply to it were added on
top. Elsewhere upstream's code was taken and ours ported onto it.

### Dropped: upstream already does it

- PPU lifter: our inline guest loads/stores (`g_vm_inl_lim`) -> upstream's
  per-function memory snapshot (`PPU_MM_SNAP`); our r1 stack accessors ->
  its one-compare range check; `--max-chunk-lines` -> `--chunk-lines`;
  callback stacks (64-slot), the atomic lwmutex, the real-PRX import block.
- SPU lifter: our second-code-segment lifting (`extra_segments`) -> upstream's
  `text_image`, which lifts every executable PT_LOAD (hkp_collide's high
  segment: 29 functions, ours 26). MFC slot release in `spu_run_lifted_job_abi`
  is upstream's too.
- cellVdec core: handle never 0, QueryAttrEx/OpenEx, 16-deep queue with BUSY,
  drain on idle, log gating -- upstream's. FFmpeg is loaded at run time
  (`PS3RECOMP_FFMPEG_RUNTIME`, libavcodec.so.60/libavutil.so.58; turned on by
  CMake when the headers are found; `PS3RECOMP_FFMPEG=0` turns it off) instead
  of linked through pkg-config; our swscale conversion -> `vdec_store_picture`.
- cellSaveData: upstream registers the ctx handlers itself
  (`cellSaveData_register_ctx_handlers` from `ppu_sysprx_register`); our
  `ps3_savedata_register_ctx` is gone. `PS3_SAVEDATA_ROOT` still wins over
  upstream's `PS3_GAMEDATA_ROOT`.
- cellGcm NV3089: upstream's row copy (`gcm_row_copy`) with the 12.4 source
  origin -- ours computed it 256x too small. Our engine hooks stay around it.
- cellPad: upstream's freshness block, SDL hot-plug, XInput (loaded at run
  time) and rumble backends; our virtual pads sit on top of them.
- Windows/macOS backends (`rsx_live_draw.c`, `rsx_d3d12_backend.c`,
  `rsx_metal_backend.m`), `cellResc.c`: upstream's, wholesale. Not built here.
- Every `__atomic` log counter -> upstream's `PS3_LOG_FIRST`; our
  `runtime/ps3_thread_name.h` -> `include/ps3emu/thread_name.h` (which now
  names threads on Linux too); `sys_time_get_timezone` -> upstream's.

### Kept (ours ported onto v0.18)

- SPURS: Taskset2, task-id ownership and slot states, event-flag attach,
  barrier, LFQueue detach (cellSpurs.c); our kernel/policy (`spurs_kernel.c`,
  `spurs_policy.c`); taskset scheduling in spu_workload.c
  (`spu_taskset_header_committed`, `spu_num_claim`, the 0x04 -> 0x01 TryJoin
  hook). Added from upstream: `cellSpursQueueSize`, `GetTasksetInfo`,
  `TaskExitCodeInitialize`, the decrementer overflow at 231 s, RdMachStat
  interrupt-enable, the list-stall ack guard.
- SPU runtime: our striped lock-line coherency, context, channels and
  float/int helpers (they pass upstream's `test_spu_float_exact`,
  `numeric_semantics`, `helpers`, `shufb`), with shims for upstream's API
  (`spu_coh_watch_add/remove`, `g_spu_coh_bits`, SDCRZ, stack-reset hint).
  The lifted SPU code now emits upstream's `SPU_DIRECT` tail calls, but
  `spu_direct_init` is not called: they stay off, as before.
- Draw engine and Vulkan backend: ours, plus upstream's
  `rsx_draw_engine_note_flip`, `_guest_read` and its 5-argument
  `_guest_wrote(loc, off, pitch, w, h)`, DXT textures never matched against a
  colour target, and the Vulkan present entry points host_posix.c needs.
- Fragment decompiler: rebuilt on upstream's, with ours re-applied
  (`rsx_fp_decompile_buffered_ex2`, half/full aliasing, `RAW8_HALF2`,
  `rsx_fp_alpha_ref` for float targets, UNNORM, CUBE_BANK, WPOS_SCALE, our
  gamma/expand rules on the ex2 path). Texture layout keeps our crossbar
  model (X16 presents {R,1,R,1}; two upstream tests changed to expect it).
- cellGcmSys: our subchannel filter (the bloom fix) beside upstream's
  `wdiag` gate; REPORT_LOCATION_MAIN at IO 0x0E000000 + offset; park waits
  (`GCM_PARK_PATIENCE_MS`), `GCM_SEMA_TIMEOUT_MS`; NV3089_GATE/DBG tooling;
  `GCM_VBLANK_EACH` (work/patches/pending/vblank_count.diff, now in the patch).
- cellVdec MPEG-4/DivX (types 2 and 5) through FFmpeg's MPEG-4 decoder, the
  N-VOP repeat, CellVdecDivxInfo; cellAdec AC3 and `cellAdecOpenEx` on the
  run-time FFmpeg table; cellFont (ours, with its own
  `cellFontInitializeWithRevision`); `PS3_SYS_LANG` read before upstream's
  `PS3_LANG`/locale chooser.
- PPU runtime: line-granular reservations and store re-check
  (`VM_WRITE_COH`), the hashed NID index, real lwcond, `--local-regs`,
  `__restrict` ctx (ppu_native_entries.py's header regex still matches).

### Behaviour differences seen

- **The guest heap moved.** v0.18 puts user memory (sys_memory_allocate) at
  0x10000000-0x20000000 and the mapper at 0x40000000+; v0.13 had user memory
  at 0x40000000. Every heap address in the logs and in the notes above
  (0x436F9040 semaphores, 0x452F8A00 zlib queue, ...) is now about 0x2F000000
  lower (the zlib queue is 0x162F7180). boot_past_fios.sh finds the queue by
  its init line now. `PS3_MEM_ALLOC_BASE` moves it within 0x10000000-0x30000000.
- `sceNpManagerGetAccountRegion` answers SCE_NP_ERROR_OFFLINE (upstream's;
  it was "us"). The game does not seem to care.
- With the draw engine on and no vtable backend, upstream hands
  `rsx_process_method` only the methods with effects outside rsx_state (#466),
  so the `[RSX] DRAW_*` lines came from nowhere; gcm_3d_method now logs the
  first draws itself (`... (draw engine)`), which first_draw.sh reads.
- PPU lift: 76,362 functions (was 76,372: 11 switch cases around 0x607340
  are now `goto` labels inside their function; one new function at
  0x126BF84 in embedded SPU bytes). SPU lift: more functions per task
  (hkp_collide 1769 -> 1894).
- `[cellVdec] DecodeAu` lines still print (~500 a race).
- Speed: the same. Old and new first_race.sh side by side on the busy host
  (load ~30): both PASS (780 / 771 s from the first save to the end), whole
  run 16.1 / 16.2 fps average, in race (frames 7200..12960) 16.0 / 16.0 fps
  average, median 15.2 / 16.0. (Those two binaries had PS3_SIM_60 but not
  GCM_VBLANK_EACH.) The final build-vk alone at load ~12: SLOW=1 run_all.sh
  8 of 8 PASS, first_race 687 s from the save, 20.9 fps average. The black
  box under the in-race button prompt is in both builds' pictures.

### Not carried / open

- Upstream engine features our engine does not have: 3D textures, user clip
  planes, scissored CLEAR_SURFACE (matters for split-screen, milestone 5),
  antialiased surfaces at sample resolution and AA WPOS, border colour,
  FP16 targets read by 2D transfers, `RSX_ENGINE_PASS_DUMP`; Vulkan present on
  its own command buffer, D32S8 depth readback, cached-memory readback.
- Upstream unit tests that do not build against the patch (they test
  upstream's engine, coherency or taskset internals, or are single-file
  fixtures that lack our `ppu_memory.h` hooks): e.g. test_color_feedback_engine,
  test_nv3089_engine_read, test_spu_lockline_peer's lock-free section,
  test_spu_taskset_elf_flags, test_spurs_taskset_bits_atomic, the
  memory-domain/mmapper fixtures. The tests for our code pass:
  test_spurs_taskset2 (110/0), test_spurs_taskset_signal (23/0),
  test_fp_decompiler (126/0), test_texture_layout, test_pad_virtual (997/0),
  test_vdec_handles, test_savedata_fixed, test_cellfont (56/0),
  test_spu_coherency (204/0), the PPU lifter self-tests (test_ppu_lift
  1533/0), and work/tests/codec (evologo_60.avi: 300 pictures; AC3
  correlation 1.000000 against the ffmpeg CLI).
- Folded in after the switch-over (from work/patches/pending/): memleak.diff
  (the per-thread SPU caches -- spu_step's decode cache, spu_lookup's
  tl_lookup -- go back to a pool when their thread exits instead of leaking:
  race RSS flat at ~1.3 GB instead of +25 MB/s) and spu_task_pool.diff (a
  worker pool for async SPURS task runs, on by default, PS3_SPU_TASK_POOL=0
  turns it off). With all three pending diffs in: SLOW=1 run_all.sh 7 of 8
  (one main_menu freeze: evologo closed, maintitles never opened -- the
  "logo that never ends" shape, no `[ppu-native] event` line; sim_speed
  passed on its own afterwards). main_menu.sh 12 more times on that build,
  six with PS3_SPU_TASK_POOL=0: 5 of 6 PASS either way, each failure a menu
  press that moved the panel too little (DOWN 11 / 17), no freeze.

## PPU/SPU performance, round 2 (2026-11-07)

Measured on a host shared with other games and builds (load average 25-55),
so frame rates are only comparable between runs made side by side. Scripts
and runs: work/runs/ppu2 (measure2.sh boots with drive.py, retries a corrupt
boot, then takes threads.py, perf of the process and perf --call-graph of
the main thread; mt.py splits a thread's samples by kind).

### Where a race frame goes

- Main PPU thread ~45% busy. On CPU: lifted game code 81%, coherence 8.6%
  (notify_spus, ll_take), dispatch 7.5% (ppu_lookup 3%, ps3_indirect_call
  1.7%), no single hot game function (the top ones are 1-2% each).
- Its waits (`PS3_MAIN_WAITPROF=1`, `PS3_LOCK_STATS=1`): the job-batch event
  (the game's spinlock wait at 0xFF5AFC, via the batch's Wait 0x673AB8)
  14-35% of wall time depending on load -- call sites 0x5D3100 (the most),
  0x45ADA8, 0x614E9C, 0x662F44, 0x76538C; sys_event_queue_receive at
  0x8C2294/0x8C18FC ~10%; usleep at 0x6BCB10/0x6BCB6C 2-5%. JobBatchHandler
  (thread 3) finds finished batches by polling with usleep(50) (~175 us a
  turn), so detection adds only ~1-2 ms a frame.
- SPURS: the five SPU threads 47-57% busy (`SPURS_BUSY_STATS=1`). Job time:
  renderablejob 19%, pvsjob 15%, vehiclephysics 8%, dxtblendjob 6%,
  hierarchyevaluate 6%; runtime overhead in those threads ~16% (DMA copies,
  overlay checks on GETs, dispatch).
- `SPURS_IDLE_SPIN_US`: the idle spin does not show in race profiles;
  `SPURS_IDLE_STATS=1`: 31k idle sleeps per 10 s, 5.5k ended by a notify,
  6-17 by the 1 ms timeout with work waiting, 1.5k found while spinning.
  0, 50 and 200 made no measurable difference; left at 200.

### MultiStream: a livelock, not audio work

The MultiStream core (task image 37) calls LS 0x21D20 (`bisl` at 0x6E28),
which is in the task's bss: the core reads the firmware's
/dev_flash/sys/external/flashMP3.pic into it at run time (vaddr 0 at LS
0x1A880, its code segment 0x1A900..0x2A810, the entry 0x74A0 = 0x21D20;
checked byte for byte against a local-store dump, `SPU_INTERP_LSDUMP_PC`).
lift_spu.py had 0x21D20 as an extra task entry; lifted from the ELF it is an
empty function, so spu_drain_call dispatched it, the pc did not move, and the
thread spun in spu_drain_call / spu_lookup for good: 100% of a core and no
audio mixed.

- `SPU_LIFT_DISABLE_AT=<img>:<hex lsa>[,...]` (runtime): leave single lifted
  entries to the interpreter (37:21D20 here). Interpreted, the decoder kept
  the core busy (spu_step, and ~40% in spu_lookup misses).
- lift_spu.py `FW_LIFTS` lifts flashMP3.pic from the user's dev_flash (found
  as main.cpp finds it; skipped when absent) as image 263 at LS 0x1A900, and
  registers it with `spu_overlay_register_bytes_at` (spu_channels.c): a
  fixed-address lift whose reference bytes are a copy in the binary, since no
  guest EA holds them. `SPU_FW_LIFT=1` makes it resident when its bytes are
  loaded; it then owns 0x21D20. TASK_EXTRA_ENTRIES' 0x21D20 is to go once
  that is the default (a check now rejects extra entries outside a task's
  executable bytes; 0x21D20 is the one allowed exception).
- `SPU_OVL_REVALIDATE=1`: the core reads each stream's 48-byte descriptor
  over the head of the first DSP plugin (LS 0x37000), which evicts the
  plugin's lift, and puts the head back from its own copy with stores, not a
  DMA. A branch that misses inside a fixed-address lift's range now checks
  whether local store holds the lift's bytes and makes it resident again.
  Interpreted DSP code went from ~110M steps per 10 s to none. The plugins
  are placed in consecutive slots from 0x37000 (text segments, 0x80-aligned),
  so which plugin sits where depends on the effect chain; only i3dl2@0x37000,
  multi_compressor_td@0x37480 and filter@0x39800 are lifted.
- With all of it the core is still ~85-95% of a core, all lifted code now
  (the filter plugin alone a quarter), in the headless runs. It is its own
  thread, off the frame's critical path.

### Job modules in a second code buffer

The WWS job manager packs job modules into local store: jobmod_9c4578e272ec
(52 KB, position-independent: `brsl` to get the pc, `ila` offsets) ran at LS
0x8000 while another module held 0x4000. Its lift exists only at 0x4000, so
those jobs ran on the interpreter, and an interpreted run inside a job module
never came back to lifted code (spu_lifted_lookup did not look at the resident
overlay): whole jobs interpreted, 8-12M SPU steps/s in the race, on the job
batches the main thread waits for.

- lift_spu.py `JOBMOD_ALT_BASES` lifts listed modules again at other buffers
  (9c4578e272ec at 0x8000, image 264); registered by (signature, base)
  (`spu_overlay_register_sig_at`), matched by the pc (`spu_overlay_match_pc_at`:
  the PM's codeBase field at LS 0x1320 still says 0x4000). `SPU_WWS_ALT_BASE=1`.
  `SPU_WWS_BASE_LOG=1` prints each (code buffer, module) pair a run uses.
- `SPU_INTERP_REJOIN_OVL=1`: the interpreter rejoins the resident overlay's
  lift, after checking that the module's head is in local store at its base.
- Together (side by side, load 55): interpreted steps 8.1M/s -> 0.1M/s, SPURS
  busy 56-57% -> 47-51% for the same work.

### Smaller changes

- `SPU_COH_WFILT=1`: watch counts per hashed line, so a notify for a line
  nobody watches skips the watch list (40% of notify_spus was that walk:
  one cold node per parked SPURS task).
- `PPU_LKC_BITS=<8..16>` (default 11): the per-thread indirect-call cache is
  heap-allocated now (was 32 KB of static TLS in every thread). 14 bits was
  within noise of 11 (ppu_lookup 3.0% vs 3.4% of the main thread).
- `PS3_MAIN_WAITPROF=<tid>`: a thread's wall time inside HLE imports and lv2
  syscalls by (NID or syscall, lr), every 10 s. `PS3_LOCK_STATS` names the
  caller of the job-batch Wait instead of the Wait.
- Kept from round 1 after validation: ppu_loader.cpp's `g_resv_unnoted`
  (`PPU_RESV_BREAK_LINE=1`: always scan) and the claim checks in the notify
  loops (`SPU_COH_LIVE=0`: off).
- Validation: main_menu.sh x2 and first_race.sh PASS with SPU_FW_LIFT=1
  SPU_COH_WFILT=1 (work/runs/ppu2/v1_*: race over 745 s after the save);
  with all five switches on: main_menu.sh x2 PASS (v2_*), first_race.sh
  3 PASS (hC1, hC2, hW1) and 1 "physics stopped" (v2_fr).
- The "physics stopped" hang (Havok tasks created by CreateTask, never
  dispatched; everything else idle) came 5 times, every time with round 1's
  uncommitted resv/claim changes on (bin_b x3, v2_fr, hA1); 0 of 3 races
  without them (hB1, hB2, c4). Those changes are also what makes the main
  thread cheap: side by side at load ~26, the main thread was 68-72% busy
  with them off against ~45% with them on at similar loads. So the bug is
  worth finding rather than the change dropped. `SPU_TS_WATCHDOG=<s>` dumps
  every tracked taskset's bitsets, the runtime's run bits and the Taskset2
  records when no task has started or exited for <s> s -- run races with it
  until one hangs.
- Side by side at load ~26, both without round 1's changes: defaults 17.8-20.1
  fps, main thread 71.5% busy (~37 ms a frame); all five switches 16.7-18.6
  fps, 68.4% (~39 ms), job-batch waits 11% of wall against 18%. The fps
  difference is within the noise of this host.

### Open

- 60 fps is not in reach by trimming: the main thread's own work is ~24-30 ms
  a frame at these loads, spread over the whole game. What would move it: the
  lifted PPU code itself (FPRs and vector registers live in ctx memory, every
  store checks the coherence bitmap twice), and the SPU job code that the
  main thread waits for (renderablejob, pvsjob).
- Seen once (integ6 SLOW, sim_speed's race, ~81 s of race clock): a host
  SIGSEGV on the main PPU thread in lifted func_00911128 (callers 0x8FDE10,
  0x8FED40), a 16-byte vector store to guest EA 0xD0000000 -- the guard page
  of the first guest thread stack (vm_stack_allocate puts a one-page guard
  under each stack, from VM_STACK_BASE 0xD0000000 up; stack sizes are the
  requested ones rounded to a page, 16 KB minimum, as lv2). The main thread's
  own stack is at 0x0FEFxxxx, so this was a pointer into another thread's
  stack area, not an overflow of its own. Not seen again.
- More DSP plugins and job modules at the slots and buffers a run uses
  (`SPU_INTERP_STATS=10` names the pages still interpreted).
- Boots with an empty hdd1 corrupted the UI timer tree (func_005BE4C0 pushes
  child/sibling links on a 33-slot stack at this+0x3A4 with no bound, so a bad
  link fills memory up through OPD 0x01430BD0) in ~9 of 10 boots on the
  00:24 tree; the 00:43 tree boots clean. With the hdd1 pre-filled (the four
  fonts, empty cache/BCES00484439898/) it never happened.

### The Havok "physics stopped" hang (open, 2026-11-08)

first_race.sh's "no Havok task for 120 s". About 1 race in 7 on a quiet host.
Under host load (40-130), or with the game pinned to 4 cores (`taskset -c
0-3`), it becomes most races, often at the same point (~245-280 s after the
first save). Three shapes at the stall (SPU_TS_WATCHDOG=20 dump):

- A: hkp_integrate (img 35) hits `stop 0x3FFF` at LS 0x19454 (Havok's
  HK_BREAKPOINT, the default of a switch on a schema type byte). Its solver
  buffer is laid out [accumulators | elements | schemas] from the task
  descriptor. The job's writes run past the end of a region into the schemas
  in its own LS, while memory still holds the right schemas. So the solve
  works on more items than the layout made room for.
- B: an integrate task in an endless loop (LS 0x13098/0x13204), or no task
  running at all. The other Havok tasks wait on semaphores in line 0x146F7880.
- C (most common): main waits in syscall 130 on queue 16 (lr 0x7820EC, the
  raycast batch semaphore 0x20 at 0x146F7840) and every Havok task is idle.

Related: a host SIGSEGV reading guest 0xD0000000 in Havok PPU code
(func_00910478, func_00911128). Havok data is inconsistent on both the PPU
and the SPU side.

Ruled out:
- The SPU lifts. SPU_DIFF lifted == interpreter on img 33/35/36, 600/600, with
  a control that interprets the live run too.
- A PUTLLC lost update. SPU_PUTLLC_CAS commits per doubleword and logs any
  store landing after the compare: zero in a hung run.
- SPU_COH_UNMARK=0, SPU_FP_MXCSR=0, PS3_SIM_60=0: all hang.
- Fence lifting: sync/lwsync/eieio/isync become atomic_thread_fence.
- Task-slot reuse: PS3_HAVOK_TRACE shows none.

Not a predictor (counted in 73 hunt logs): libspurs' create heqi at
integrate LS 0x126D8 (in most passes too), and raycast DMA to EA 0xFFFFFFFF.

Tools, all default off:
- `SPU_ASSERT_DIAG=1`: on a stop 0x3FFF, dumps registers, the last 64
  transfers, and the small descriptors as LS copy vs memory.
- `PS3_HAVOK_TRACE=1` (port/ppu_native.cpp): a ring of addJob, the Havok
  semaphores, the raycast build/wait, task creates and returns.
- `SPU_LINE_WATCH` (4 lines).
- `SPU_DIFF` + `SPU_DIFF_CONTROL`.
- `SPU_PUTLLC_CAS`.
- work/runs/ppu2/perfwatch.sh (perf of the live game at a stall) and
  hunt.sh.

## SPU job speed (rsx2, 2026-11-07)

Side-by-side 1P races (work/runs/rsx2/spu: `ab.sh` two races at once,
`prof.sh` with perf, `tlan*.py` / `jobtime.py` read `PS3_TIMELINE`), load
average 35-65 throughout, so only same-moment pairs are compared.

### Where the RSX and the main thread wait

- The RSX's 0x4B0/0x4C0 acquires come right after the frame's first two
  blits: the RSX is waiting for the next frame's work, 60-135 ms a frame at
  load ~55. Through those waits the five SPURS SPUs are 42-78% idle, so it is
  a dependency chain, not SPU throughput.
- Main-thread batch waits (`lkw0`/`lkw1` timeline events, work/port/ppu_native.cpp
  nat_lock; on only with PS3_TIMELINE) and what the SPUs run in them:
  0x662F44 -- renderablejob on all five SPUs (15-27 ms); 0x45ADA8 -- one
  screenrushjob for ~15 ms with four SPUs idle, then pvsjob on all five;
  0x614E9C -- one hierarchyevaluate, ~14 ms.
- Hops (load ~55): kick -> SPU wake (knotify -> kwake) p50 108 us, p90 1.8 ms;
  only one wake in the window came from the 1 ms idle timeout. Last job-list
  row PUT -> main thread awake p50 0.5 ms, p90 6.9 ms. The tails are the host
  scheduler at this load, not missed wakes.
- `kspu` samples carry the instance in the top byte of `a`: a second SPURS
  instance (0x1B74FB80, one SPU) shares SPU number 0 and its g_spurs_idle_mask
  bit. Filter on it.

### Changes

- **SPU_FW_LIFT / SPU_WWS_ALT_BASE were never on.** spu_workloads.c reads them
  in its static constructor, before main()'s env_default. lift_spu.py now
  emits them as "unset = on" (=0 off). MultiStream went from 7M interpreted
  SPU steps/s to 0.1M. Audit: of main.cpp's env_default names no other one is
  read before main(); SPU_OVL_OWNER (read through spu_overlay_owner_last from
  the same constructor) has the same trap but is not defaulted.
- **`SPU_FP_MXCSR=1`** (default off; runtime/spu/spu_helpers.h `SPU_FP_MX`,
  libs/spurs/spurs_kernel.c kspu_main). On AVX2 hosts without AVX-512 the
  exact single-precision forms widen to double and back. With the switch each
  SPURS kernel thread sets MXCSR to round toward zero + FTZ + DAZ and
  fa/fs/fm/fma/fms/fnms run as native single-precision ops; a lane with an
  exponent-255 operand or a result at/above FLT_MAX (the result shows it as
  Inf/NaN/FLT_MAX) sends the vector to the exact path under round-to-nearest.
  The double forms, frds and the csflt/cuflt fix-up put round-to-nearest back
  around themselves (SPU_FP_RN_BEGIN/END), and the interpreter runs each step
  under it (spu_interp.c spu_step). Other threads (PPU, task threads) are
  never switched (`spu_fp_fast` is thread-local). Test:
  runtime/spu/tests/test_spu_float_mx.c, 2M random+edge vectors per op
  bit-identical to the exact forms (clang and GCC); micro-benchmark 5.2 vs
  24.3 ns per four ops. In the race: SPU job time 67.5 -> 58.6 ms a frame
  (vehiclephysics 5.8 -> 3.6, renderablejob 21.3 -> 18.4); frame time within
  noise (main-thread bound). Validated: main_menu x4 (one attract-mode flake
  also seen without it), SLOW race_intro, splitscreen.
- `SPURS_NSPUS_MIN=8`: the extra SPUs (6, 7) spin at 100% and the game never
  reaches a race -- the job manager takes at most 6. `=6`: no gain in two
  swapped pairs (17.4/16.9 and 10.6/11.0 fps).

### What is left

- Per job the lifted code is register-pressure bound: in renderablejob's hot
  functions ~35% of samples are xmm spills/reloads (128 SPU registers on 16
  host ones), the rest LS loads/stores (one pshufb each) and shufb. The
  SPURS-thread runtime overhead is ~33% (gm_copy_from 5% -- memory bound --,
  lock-line reads 5%, overlay checks on GETs ~5%, dispatch ~5%).
- The SPU MLAA tasks still run every frame with native MLAA on (~7% of the
  process in their lifted code, more with their runtime): skipping their work
  needs their completion protocol emulated (the barrier hang history).

## Audio and controllers in real play (rsx2, 2026-11-07)

### Audio

- Tools: `AUDIO_WAV=<file>` (the final mix, raw f32le stereo 48 kHz),
  `PS3_AUDIO_MUTE=1` (silent at the device, which still paces the mixer --
  real PulseAudio/PipeWire runs without sound in the room), and new
  `PS3_AUDIO_STATS=1` (cellAudio.c, every 5 s: blocks mixed, submits that found
  under one block queued, min queue, longest mixer gap, port blocks the
  producer left empty). work/runs/rsx2/spu/mscpu.sh: MultiStream's CPU per
  produced block.
- The game: port 0 is MultiStream (8 ch, 16 blocks, no notify queue -- it
  paces off the read index); movies open port 1 (8 ch, 8 blocks) with a notify
  queue. The mixer holds 187.5 blocks/s; device underruns are rare (0-4 per 5 s
  at load 60-80).
- Movies: complete (0-16% empty blocks during the intro movie).
- Menu and race: chopped. MultiStream fills only part of its ring -- bursts of
  ~20 blocks, then ~16 empty (menu: 42% empty; race: 60-70% at load 80). Its
  task thread is at a full core in real work (DSP plugins 54%: filter, i3dl2
  reverb, compressor; the MultiStream core 36%; no spinning): it is too slow,
  not starved of wakes. At load ~60: 8.7 ms CPU per 5.33 ms block (163% of a
  core); with `SPU_FP_MXCSR_TASKS=1` 6.1 ms (114%), side by side empty blocks
  64% -> 45%. Whether a quiet host's full core covers it is still to measure.
- `SPU_FP_MXCSR_TASKS=1` (with SPU_FP_MXCSR=1; runtime/spu/spu_workload.c
  spu_async_run): taskset task threads switch to the fast single precision
  too. Validated: main_menu x2, SLOW race_intro, splitscreen.
- Next lever if a quiet host still falls short: the DSP kernels themselves
  (msdsp_filter's FIR loop: load, fm, 2 shufb, 2 fma, store per 4 samples).

### Controllers

- cellPad's SDL path: a pad takes the first free port (0-3) in the order it
  appears, a reconnected one gets its old port back (by GUID), a fifth waits
  for a free port; triggers are pressure (SDL 0..32767 -> 0..255, digital bit
  from ~9%), sticks 0..255. test: libs/input/tests/test_pad_sdl_virtual.c
  (SDL >= 2.24 virtual game controllers through the real cellPad.c; it ignores
  any real pad on the host with SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT --
  this host has an Xbox 360 pad on js0, which every game run opens as player 1).
  SDL virtual trigger axes span -32768..32767 (rest -32768).
- The "Controller & saves" tab (work/port/ui.cpp) lists every pad with its
  player number (`cellPad_port_of_instance`, cellPad.c), held buttons and
  L2/R2 %, under SDL's joystick lock; on the start screen it starts SDL's
  controller support itself (it showed "none found" there before).
- `PS3RECOMP_UI_KEYS` also takes `click:X,Y` (window pixels); ImGui may need
  it sent two or three times.

## PPU codegen performance (2026-11-07)

What the lifted PPU code costs on the main thread, and new lifter options
that make it cheaper. All behind flags: `ppu_lifter.py` with the old flags
writes byte-identical output (relifted and diffed against work/src/recomp).

### Measuring

`work/codegen/bench.py NAME BIN [track]` boots straight into a quick race
(all_tracks.sh's patched gamestates.xml, union_v1 by default), holds R2 and
measures the main thread between two frame dumps (exact frame counts):
CPU ms/frame, `perf stat` events, or with `PERF=1 PERF_REC_EV=cycles:u,instructions:u`
a perf profile that `work/codegen/cat.py` splits per frame into lifted code,
waits (sys_timer_usleep, lk_wait, ...), coherence, dispatch/HLE. Boot to the
race HUD ~45 s, a run ~2 min. The host ran at load 40-100 throughout: cycle
counts are only comparable side by side; instruction counts are stable to
~2%.

A control with the same runtime objects is linked from another build's
lifted objects (`work/codegen/hyb.py LINKCMD OUT BUILDDIR NNN=OBJ...`; with
`--chunks 140` the chunk cut depends only on the function list, so any mix
of two lifts' chunk objects links).

### Where it goes

- Main thread, race, default lift: lifted code ~75M cycles and ~70M
  instructions a frame, IPC ~0.85; `ic_fetch_stall` on nearly every cycle and
  ~38 L1i misses per 1000 instructions. It is frontend-bound: per ~40 s the
  main thread runs ~2450 lifted functions, 26 MB of x86 (the top 200, half the
  time, are 5.5 MB). The default lift is ~70 bytes of x86 per PPU instruction.
- With `--chunks 140`, 10 chunks hold 72% of the main thread's lifted
  samples and 20 hold 93%.
- What made the code big: every guest store ~25 instructions of checks (span,
  two diagnostic globals, a NULL test and the bitmap pre-check, the bitmap
  post-check) with an out-of-line slow path whose call clobbered every SSE
  register, so float and vector values lived in stack slots; FPRs and VRs in
  ctx (re-read after every store); SNC's `addic` for stack addresses (~12% of
  all PPU instructions) whose dead carry stayed live in `_xer` to the next
  write-back; a ~40-byte trampoline drain at every call site; the memory
  model recomputed at every function entry.

### The options (`--local-regs` plus)

- `--local-fprs`, `--local-vrs`: FPRs and VRs as C locals, the same
  dataflow as `--local-regs` (written back before calls and exits, reloaded
  after calls).
- `--split-xer`: XER as two locals, CA and the rest; an addic's carry is
  then dead unless something reads it. (`--split-cr`, CR as eight field
  locals, made the code bigger; not used.)
- `--fast-mm`: the memory-model copy from a per-unit cache filled once; a
  store tests the runtime's `g_spu_coh_bitmap` array directly (no pointer,
  no NULL test; zero until an SPU reserves a line); the diagnostic globals
  dropped from the inline store (their probes already force the span to 0);
  lvx/stvx as one 16-byte access through the copy; the trampoline drain out
  of line; the out-of-line accesses through register-keeping thunks.
- `--lean-fp`: the FP helpers test the result for NaN once and look at the
  operands only when it is one. Same bits (11760 operand combinations incl.
  NaN payloads/signs, GCC and clang).

The thunks: the cold paths call `vm_*_cold`, which callers see as clang's
`preserve_all` (everything but RAX/R11 kept, YMM0-15 whole). They are
naked asm, because clang 18 ends an AVX `preserve_all` function with a
`vzeroupper` before `ret`, wiping the upper halves of the YMM registers it
has just restored. That bug cost a day: the first build guest-read NULL in
func_009649C4 on every boot; found by linking mixes of the new and the
default chunk objects (chunk 048), then header variants. A void thunk must
keep RAX too (callers keep values in it across a void preserve_all call).
`tools/test_ppu_fast_mm.py` checks both against a callee that clobbers every
caller-saved register.

### More options (code size, behind `--fast-mm`)

- `--coh-bytemap`: a store tests `g_spu_coh_lines` (runtime/spu/spu_coherency.c:
  one byte per 128-byte line, 32 MiB of bss) instead of extracting a bit:
  one `cmpb` before and one after. The runtime keeps the bytes equal to the
  bitmap: each of the three bitmap writes (coh_mark_line's fetch_or,
  coh_line_dead's fetch_and and its put-back) is followed at once by the same
  byte store, before the process-wide barrier, and coh_mark_line's fast path
  also requires the byte. Always maintained; test_spu_coherency 204/0.
- `--stack-fast`: loads and stores at r1 + displacement (52% of stores, 27%
  of loads) skip the span test and both reservation tests. Checked before:
  over a boot, the menus and a race, no line of a PPU stack (0xD0000000+,
  the main stack 0x0FE..0x0FF) was ever in the reservation bitmap (sampled
  4x a second from inside the process by an LD_PRELOAD thread; ptrace is
  blocked on this host). The accesses stay relaxed atomics: the first
  version used plain memcpy loads and the boot hung before evologo (a
  guest loop polling a stack object had its load hoisted).

### Results

Same runtime objects, lifted chunks swapped (hyb.py), union_v1 race,
CPUs 0-7,16-23, one game at a time, per frame of the main thread:

| lift | main CPU ms/frame | lifted cycles | lifted insns |
|---|---|---|---|
| default | 23.9 / 20.8 | 69.2M / 59.2M | 69.8M / 61.1M |
| v3 (`--local-fprs --local-vrs --fast-mm --lean-fp --split-xer`) | 17.7 / 17.8 | 51.4M / 50.9M | 54.8M |
| v3 + PGO | 15.7 / 16.5 | 44.6M / 46.6M | 53.6M |

(load ~20.) A second series on the byte-map runtime, load 25-40:
default 17.8 / 15.1 ms, 68M / 60M lifted insns; v3 15.3 / 12.9 ms, 55M /
53M; v3 + byte map on the 20 hot chunks 11.7 / 13.5 ms, 43M; v5 (v3 +
`--coh-bytemap --stack-fast`, full) 12.4 / 15.5 ms (the second during a load
spike), 39.5M / 38.0M. Lifted instructions -42%, lifted cycles ~-25% for v5.
Binary 656 MB (default) -> 553 MB (v3) -> 463 MB (v5).

PGO: an instrumented build of the lifted chunks only (`-fprofile-generate`
through work/codegen/ppu_cxx_pgo.sh; libclang_rt.profile from
libclang-rt-18-dev, which the host lacks, linked last), three races
(union_v1, mainline_v1, dogpatch_v1) with work/codegen/pgo_dump.cpp
writing the counters every 20 s (the game is killed, and g++ linking means
no `-u__llvm_profile_runtime`, so the file name is set by hand), llvm-profdata
merge, then `-fprofile-use -fsplit-machine-functions`. About -10% lifted
cycles on top of v3. The profile belongs to one lift: any lifter or header
change needs a new instrumented build and training run (~2.5 h on this host).

Final series (2026-11-08/09), same runtime objects (build-v5), union_v1:

| | main CPU ms/frame | lifted cycles/frame |
|---|---|---|
| default, load ~10, x3 | 14.7 / 13.3 / 14.6 | 45.8M / 44.3M / 46.6M |
| v5, load ~10, x3 | 10.3 / 11.4 / 11.0 (-24%) | 29.3M / 31.3M / 31.7M (-32%) |
| default, load ~40, 4 pairs side by side | 29.8 / 33.3 / 29.0 / 30.3 | |
| v5, same pairs | 23.0 / 22.8 / 22.3 / 21.8 (-26%) | |
| v5, load ~45, 3 pairs side by side | 26.7 / 28.7 / 24.9 | 53.3M / 53.0M / 47.8M |
| v5 + PGO (trained on mainline_v1, dogpatch_v1), same pairs | 23.8 / 23.4 / 23.0 (-12%) | 48.1M / 46.5M / 44.8M (-10%) |

Validation: test_ppu_lift 1533/0, test_ppu_local_regs, test_ppu_fast_mm
pass, test_spu_coherency 204/0. v3: ALL_TRACKS 39/39, SLOW first_race PASS,
run_all 7/8 (festival's own embedded first_race failed once; first_race
alone passed). v5: ALL_TRACKS 39/39 (twice), SLOW first_race PASS,
race_intro PASS, main_menu x2 PASS (after the stack fix), reach_entry,
boot_past_fios, first_draw, first_frame, save_options, first_boot_fresh
PASS. In one series at load 45-65, 2 of 3 v5 runs and 1 of 2 v5+PGO runs
stopped presenting after the HUD (one with the SPU 0 ch-wait / NULL reads
0x0,0x10,0x13 signature that default code also shows, 2 of ~12 default
runs earlier); the next 8 side-by-side runs at load ~40 (4 default, 4 v5)
all ran.

work/tools/ppu_native_entries.py matches a native block's before/expect
lines after folding `vm_*_stk` back and cutting at an XER update, so its
func_00911128 block applies to a lift with these options too (the default
lift: unchanged, `--check` reports 0 chunks).

### Tried, measured, dropped

- BOLT (bolt-18; relink with `--emit-relocs`; Zen3 has no LBR, so
  `perf2bolt -nl`; ext-tsp, hfsort+, split-functions): iTLB misses -73%, but
  lifted code 22% slower (96M vs 78M cycles a frame, twice).
- `preserve_all` wrappers written in C (the clang bug above).
- `--split-cr`: bigger code.

## Toward 60 fps: the critical path (2026-11-10)

1P race (work/codegen/bench.py, union_v1, 30 s windows), the whole 5950X
(two 8-core CCDs, 16 cores / 32 threads) unpinned, as players run it. Runs of
one binary differ by about +-2 fps, so changes are read over pairs.

### What the frame waits on

- Causal test (`PS3_CAUSAL_MAIN_US` / `PS3_CAUSAL_RSX_US`, cellGcmSys.c:
  spin N us once a frame on that thread): +3 ms on the main PPU thread took
  56.5 -> 49.6 fps (+2.4 ms a frame, ~80% on the critical path); +3 ms on
  the RSX thread changed nothing. The frame is the main thread: ~8.5 ms of
  its own CPU plus its waits.
- Its waits a frame (`PS3_MAIN_WAITPROF`, `PS3_LOCK_STATS`): ~3.8 ms at
  0x45ADA8 (the per-frame driver func_0045AC74 waiting on an event that the
  "JobBatchHandler" PPU thread, tid 3, sets when an SPU job batch is done:
  `PS3_LOCK_WATCH_SITE=45ADA8` puts the waited word under SPU_LINE_WATCH and
  showed thread 3 releasing it), ~1.9 ms in three event-queue receives,
  0.35 ms on the lock at 0x5D3100, 0.2 ms polling a frame counter
  (0x6BC824: `while ([o+0x2700] != [o+0x5C0]) usleep(100)`).
- During the 0x45ADA8 wait the SPUs are 5.4 of 6 busy, mostly pvsjob, then
  screenrushjob and renderablejob (`PS3_TIMELINE` window, kspu samples).
  dxtblendjob, the biggest job by SPU time, runs outside those waits.
- Present intervals are not quantized to the vblank: 14-20 ms, mean 17.1 ms
  (`PS3RECOMP_VK_HITCH=0`).

### Changes

- Direct SPU tail calls (spu_drain.c): `spu_direct_init` turned them off
  ("no musttail in this compiler") because the runtime is built by GCC,
  while the lifted SPU code that reads the flag is built by clang and has
  musttail. Every chunk-to-chunk hop went through the trampoline loop
  (~750 M fallbacks per 10 s; `SPU_DIRECT_STATS=1` counts them by reason).
  On: ~0.1 M, and 54.6 -> 56.4 fps at 6 SPUs.
- Six SPURS SPUs (`SPURS_NSPUS_MIN=6`, main.cpp default): 54.3 -> 56.4 fps.
  7 and 8 hang at boot.
- PGO for the lifted PPU code (`work/pgo/ppu.profdata`, not in git; CMake
  PORT_PPU_PROFILE, scripts/build.sh): main thread 8.9-9.1 -> 8.3-8.5 ms a
  frame, 56-57 -> 58-59 fps.
- dxtblendjob natives for the DXT1 block decode (0x4358) and the min/max
  (0x4030), beside the colour-index search (0x4250): the same SPU helpers on
  the same values, so exact (SPU_NATIVE_CHECK: 0 mismatches in 47 k / 95 k
  checked calls). Less SPU time, no fps change (off the critical path).
- Diagnostics: `PS3_LOCK_STATS` also prints `[lock-hold]` (holders per
  thread, site and lock, and event Set callers with the site's top bit set).

### Tried, no gain

- `PS3_HOT_PIN=1` (a core each for main, rsx, rsx-record, SPUs 0-4 on
  CPUs 0-7): 54.6 / 55.1 vs 58.2 unpinned.
- `PS3_USLEEP_YIELD=1`: 56.3 / 56.3 vs 57.2 / 58.6.
- `SPURS_IDLE_SPIN_US=2000` (default 200): no change.

### Where it stands

58-59 fps average (1P race, whole machine). What is left on the path: the
main thread's own CPU (runtime overhead ~18% of it: notify_spus, ppu_lookup,
ll_take, reservation helpers) and pvsjob's batch (about half of pvsjob's SPU
time is DMA handling: gm_copy_from, spu_overlay_note_get, MFC).

### Havok hang fix on by default (2026-11-11)

`SPU_EVENT_DEFER=1` (ppu2's fix for the "physics stopped" shape, spu_channels.c)
is now a main.cpp default. first_race, 4 rounds of 2 on + 2 off at once on
the two CCDs (work/runs/edab): on 8/8 finished; off 6/8 (both hangs in the
most loaded round, "no Havok task for 120 s" at 498 and 611 s). 1P race fps
with it: 59.5 / 58.8 vs 59.1 / 58.1 without.

### PGO for the lifted SPU code

CMake PORT_SPU_PROFILE / PORT_SPU_PROFILE_GEN (scripts/build.sh: work/pgo/
spu.profdata when present; `PORT_SPU_PROFILE_GEN=1 PORT_PGO_RT=<libclang_rt.
profile-x86_64.a> PORT_PGO_DUMP=work/codegen/pgo_dump.cpp` for the training
build, which only recompiles the SPU sources). Trained on union_v1,
mainline_v1 and dogpatch_v1 races (counters reset at 30 s, written every
20 s), merged with llvm-profdata-18. Measured neutral to slightly positive
(58.6 vs 58.4 fps over two pairs): the SPU work on the critical path is a
small part of the frame.

### SPU MLAA work skipped under native MLAA (2026-11-11)

With native MLAA the engine leaves the frame's copy into the MLAA buffer out
of guest memory and draws its own MLAA over the target; the five EDGE MLAA
tasks still reworked the buffer's stale bytes every frame (largest single
module at 1440p: ~10% of the process, on the RSX's chain). Their pass
(task_edge_post_mlaa 0x4740: computation on LS only; the DMA and the start /
barrier / completion protocol are in 0x3120 around it) now returns at once
while `rsx_draw_engine_mlaa_left_out()` says the frame's copy was left out
(work/port/spu-native/mlaa_skip.c, lift_spu.py NATIVE_DIRECT;
`PS3_MLAA_SPU_SKIP=0` runs it always). A race: 65536 passes skipped, 60 run
(the frames that went the guest's way). Split screen never skips (its views
go the guest's way) and passes with the same view lumas. race_intro at 1440p
with and without: same picture quality. 1P race at 1440p: 58.8 / 59.6 fps
vs 57.9.

### Where it stands (2026-11-11)

1P race, whole 5950X, bench.py with `BENCH_DUMP_EVERY=600` (its default
dump every 30 frames is a GPU readback plus a PPM write: a spike every 0.5 s
that play does not have): 720p mean frame 16.73-16.77 ms (59.9 fps), 1440p
16.68-16.75 ms (59.7-59.9 fps); median 16.70 ms, p99 under 20 ms. 4-player
split screen 37 fps: there the main thread waits on space in the command
buffer (usleep(30) at 0x85D1A0, 23% of its wall time) while the RSX thread
decodes and dispatches the FIFO (cellGcm_rsx_process_fifo, rsx_dispatch_method,
vertex fetch, sink_end_draw): the next thing to make faster. SLOW suite on
dc3277e: 12/12, main_menu x2.

### Real play differs from the benchmarks (2026-11-11)

user/settings.ini is applied only when a window is open (main.cpp), so every
headless benchmark ran without what play.sh gets from it. One of those
exports was `PS3RECOMP_ENG_VPULL=1` (vertex pulling, from inFAMOUS 2): with
it the 1P race at 1440p ran 46.7 fps with a 54 ms p90 (stutter), against
60.0 fps / 17.7 ms without; full-resolution particles, 16x anisotropic and
the recording thread each kept 60. settings.cpp now exports 0. bench.py
`BENCH_VIDEO=wayland` with `PS3RECOMP_VK_WINDOW=1 PS3_USER_DIR=<copy of
user/>` measures the windowed path with the player's settings.

### The second SPU MLAA pass

The MLAA task's per-frame routine 0x3120 also calls 0x5FB0 twice (through
"return address + 8" jumps that its static call graph does not show): the
pass that streams rows in and out and runs the edge kernels (0x7508,
0x8660, 0x6B90). Skipping only 0x4740 removed 24% of the tasks' work;
with 0x5FB0 too, their samples went 18207 -> 121 in a race (task threads'
total 63k -> 31k). 0x5FB0 does its own DMA and tag waits and no
synchronization (no atomics, signals or mailboxes). In boot and menus the
passes run (42540 run in a race_intro boot): native MLAA is for world
frames only.

### RSX thread: two cache fixes, and where 4P stands

- Fragment programs with patched inline constants (titles rewrite them in
  place, draw after draw) were a memo miss each time: size, structural hash,
  constants and texture mask derived again, and a new entry. Now an entry at
  the same address whose bytes differ only in the constant payloads is
  reused with its constants re-read (`PS3RECOMP_ENG_FPPATCH=0` off).
- The recording queue dropped every kept snapshot and view whenever the
  backend released anything (a snapshot made again at another size, which
  4P does all frame), and each later snapshot then waited for the queue to
  empty. The backend now names the released handles (freed_since) and only
  those answers go: 4P queue syncs 14-26 k -> 4-6 k per 10 s.
- 4-player split screen: 30-37 fps, the RSX thread busy the whole frame at
  ~2.8 us a draw over ~10 000 draws (1P: ~3.3 us over ~3 700). A draw costs
  the same; 4P has 2.7x the draws. 60 fps needs ~1.5 us a draw: the FIFO
  decode and the draw processing on separate threads (~25% of the thread is
  decode), vertex decode off the thread (~12%), and more besides.

Validation of the above (2026-11-11): SLOW suite on the build: 9/12 with
three race tests stopping on the Havok integrate hang (shape B) while other
work loaded the second CCD; on a quiet host first_race, sim_speed (48/49
windows at full speed) and festival pass, and 8/8 first_race runs (four at
once, MLAA skip on and off alike) finished. Windowed with the player's
settings (scale 2, VSync off, HUD on), 1P race: 59.6 / 59.9 fps, median
16.70 ms, p99 19.5-20.8 ms; 4P split screen 34 fps.
