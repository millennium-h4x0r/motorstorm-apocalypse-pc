/* MotorStorm Apocalypse (BCES-00484): SPU jobs whose pixel work the GPU does
 * instead.
 *
 * The mechanism, from the runtime (rsx_draw_engine.h, spu_workload.h): once a
 * job's inputs and outputs in guest memory are known (EAs, sizes, the job
 * module overlay and the LS entry of its per-tile/per-kernel function), a pass
 * here
 *   - describes them to the draw engine (rsx_draw_engine_set_native_ssao /
 *     rsx_draw_engine_set_native_lighting / rsx_draw_engine_set_native_mlaa,
 *     or a new pass added there),
 *   - can watch the job's parameter DMA (spu_dma_get_watch),
 *   - gates the SPU function so it returns at once while the GPU's result is
 *     live (spu_gate_function), and
 *   - skips the job's transfers that then carry nothing (spu_dma_skip_range).
 *
 * EDGE MLAA (PS3_NATIVE_MLAA, default 1; 0 is the game's own SPU path). The
 * race frame is tonemapped into the local target 0x02AA0000 (the picture at
 * 20,30 in it, at the dynamic resolution), copied with NV3089 to a buffer in
 * main memory, its borders filled from a 4x4 texel, antialiased there by the
 * five EDGE MLAA SPU tasks, which release label 0x480 when done, and copied
 * scaled (to 1240x680 at 20,20) into the display buffer once the RSX's
 * semaphore acquire of 0x480 is met (docs/NOTES.md "Race rendering"). The
 * display copy of frame N comes early in frame N+1. On the GPU: the copy and
 * the fills are left out, and the display copy is the engine's own MLAA over
 * the target plus a bilinear blit into the display buffer's target -- no
 * write-back of the target with its wait for the GPU, no per-pixel copies, no
 * upload. The SPU tasks still run, on a buffer nobody reads any more, and the
 * RSX still waits for them at 0x480: that wait also keeps the PPU from
 * signalling the tasks for the next frame while they are still running (the
 * boot hung now and then when it passed). PS3_NATIVE_MLAA=2 passes it anyway,
 * for measurement. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "rsx_draw_engine.h"

#define MLAA_SOURCE_TARGET 0x02AA0000u   /* local: the tonemap target */
#define MLAA_LABEL         0x480u        /* the semaphore the display copy waits on */

static void native_mlaa_init(void)
{
    const char* e = getenv("PS3_NATIVE_MLAA");
    const unsigned mode = e ? (unsigned)strtoul(e, NULL, 0) : 1u;
    if (mode != 1u && mode != 2u) {
        fprintf(stderr, "[port] native MLAA off: the SPUs antialias the frame\n");
        return;
    }
    const rsx_native_mlaa_desc d = {
        .mode       = 1u,
        .src_offset = MLAA_SOURCE_TARGET,
        .label      = MLAA_LABEL,
        .skip_acquire = mode == 2u,
    };
    rsx_draw_engine_set_native_mlaa(&d);
    fprintf(stderr, "[port] native MLAA on%s (PS3_NATIVE_MLAA=0 for the SPU path)\n",
            mode == 2u ? ", semaphore 0x480 passed" : "");
}

void port_native_passes_init(void)
{
    native_mlaa_init();
}
