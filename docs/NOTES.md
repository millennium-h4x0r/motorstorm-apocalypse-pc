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
