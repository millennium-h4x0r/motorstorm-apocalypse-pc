/* The EDGE MLAA tasks' work, skipped while the port does the anti-aliasing
 * itself (task_edge_post_mlaa, LS 0x4740; lift_spu.py NATIVE_DIRECT).
 *
 * With native MLAA (PS3_NATIVE_MLAA=1, work/port/native_passes.c) the engine
 * leaves the frame's copy into the MLAA buffer out of guest memory and draws
 * the display copy as its own MLAA over the render target; the five SPU tasks
 * still ran every frame over the buffer's stale bytes, and nothing read what
 * they wrote: ~7% of the process at 720p, ~10% at 1440p, on the RSX's chain
 * (it waits on the tasks' completion label before the display copy).
 *
 * 0x4740 and 0x5FB0 are the task's two MLAA passes over its part of the
 * buffer (0x4740 on what is in local store; 0x5FB0 streams rows in and out
 * itself and waits on its own DMA tags), each called twice a frame by 0x3120,
 * which does the start / barrier / completion protocol around them. Neither
 * holds any of that protocol (no atomics, signals or mailboxes), so returning
 * from them at once leaves every handshake as it was; the buffer they would
 * have rewritten is not read while native MLAA is armed. It is skipped
 * only while the engine reports this frame's copy as left out
 * (rsx_draw_engine_mlaa_left_out); when the frame goes the guest's way (a
 * fallback, the cool-down after it, a second view) the pass runs.
 * PS3_MLAA_SPU_SKIP=0 always runs it. */
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

extern int rsx_draw_engine_mlaa_left_out(void);

int spu_native_mlaa_pass(spu_context* ctx)
{
    static int on = -1;
    if (__builtin_expect(on < 0, 0)) { const char* e = getenv("PS3_MLAA_SPU_SKIP"); on = !(e && e[0] == '0'); }
    if (!on) return 0;
    static unsigned long long s_skip, s_run;
    if (!rsx_draw_engine_mlaa_left_out()) { __atomic_fetch_add(&s_run, 1, __ATOMIC_RELAXED); return 0; }
    const unsigned long long k = __atomic_add_fetch(&s_skip, 1, __ATOMIC_RELAXED);
    if (k == 1 || (k & 0x3FFF) == 0)
        fprintf(stderr, "[mlaa-skip] SPU MLAA passes skipped %llu, run %llu (native MLAA draws the picture)\n",
                k, __atomic_load_n(&s_run, __ATOMIC_RELAXED));
    ctx->pc = ctx->gpr[0]._u32[0];                 /* bi $r0 */
    if (ctx->host_depth == 0) g_spu_trampoline_fn = spu_indirect_branch;
    return 1;
}
