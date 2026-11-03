/* MotorStorm Apocalypse (BCES-00484): SPU jobs whose pixel work the GPU does
 * instead.
 *
 * Nothing is substituted yet: port_native_passes_init (called from main.cpp
 * once the runtime is up) is a no-op.
 *
 * The mechanism, from the runtime (rsx_draw_engine.h, spu_workload.h): once a
 * job's inputs and outputs in guest memory are known (EAs, sizes, the job
 * module overlay and the LS entry of its per-tile/per-kernel function), a pass
 * here
 *   - describes them to the draw engine (rsx_draw_engine_set_native_ssao /
 *     rsx_draw_engine_set_native_lighting, or a new pass added there),
 *   - can watch the job's parameter DMA (spu_dma_get_watch),
 *   - gates the SPU function so it returns at once while the GPU's result is
 *     live (spu_gate_function), and
 *   - skips the job's transfers that then carry nothing (spu_dma_skip_range).
 * Each pass should be off by default and switched by its own env var
 * (PS3RECOMP_NATIVE_<NAME>=1 on, 2 compute on the GPU and compare only).
 *
 * Add MotorStorm passes here once its post-processing jobs are identified. */

void port_native_passes_init(void)
{
    /* no MotorStorm passes yet */
}
