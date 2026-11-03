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
- **DSP plugins.** Found and checked only. Set `MS_DSP_LSAS` in the script to
  lift each at given LS addresses, as images 260 and up with
  `spu_overlay_register_region_at`.
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
  LS address the MultiStream core picks at run time. A lift has absolute
  branch targets, so the toolkit cannot dispatch one without knowing that
  address. Until `MS_DSP_LSAS` is filled in from a run (`SPU_MISS_LOG=1` or
  `SPU_DUMP_OVL=<dir>,<lsa>`), they run on the interpreter inside the lifted
  MultiStream task. If the core uses more than one slot, each plugin needs a
  lift per slot. libmstream itself is statically linked PPU code with no HLE,
  so it runs lifted.
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
