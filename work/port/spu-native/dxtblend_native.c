/* Native stand-in for dxtblendjob's DXT colour-index search
 * (jobmod_d6cae8b8000a, LS 0x4250; spu_lifter.py --native-at, SPU_NATIVE=1).
 *
 * The SPU function: r3 -> 16 pixels (quadwords, colour in words 0-2), r4 -> A,
 * r5 -> B. Palette C1 = fma(B-A, 1/3, A), C2 = fma(B-A, 2/3, A). Per pixel P
 * and X in {C1, B, A, C2}: S_X = (|X0-P0| + |X1-P1|) + |X2-P2|, then
 *   index = 2*((S_A>S_C2 & S_B>S_C2) | (S_B>S_C1 & S_A>S_C1)) + (S_C2>S_C1 & S_B>S_C1)
 * (the SPU's selb on a 0/1 mask only picks bit 0: a sum, not a select),
 * and r3 word 0 = OR of index << 2*i. Its callers read r3 word 0 only.
 *
 * Exact against the lift: the same single-precision operations in the same
 * order under SPU_FP_MXCSR's MXCSR (round toward zero, FTZ, DAZ), eight pixels
 * per vector. When that mode is off, or an input is large enough that a sum
 * could leave the IEEE range (where SPU and IEEE floats differ), it returns 0
 * and the lifted body runs. Checked with the job replay harness
 * (spu_channels.c spu_job_capture) on captured runs. */
#include "spu_helpers.h"
#include <immintrin.h>
#include <x86intrin.h>

static inline float f_of(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* SPU_NATIVE_CHECK=1: every 1024th call also runs the lifted function on a copy
 * of the context (same local store; it writes only its stack frame) and
 * compares r3 word 0 with the stand-in's; counts printed every 2^14 checks. */
extern void jobmod_d6cae8b8000a_e11C8_spu_func_00004250(spu_context* ctx);
static __thread int t_busy;
static int native_4250(spu_context* ctx, uint32_t* out);
int spu_native_jobmod_d6cae8b8000a_e11C8_spu_func_00004250(spu_context* ctx)
{
    if (t_busy) return 0;
    uint32_t mask;
    const unsigned long long tn0 = __rdtsc();
    if (!native_4250(ctx, &mask)) return 0;
    const unsigned long long tn = __rdtsc() - tn0;
    static int s_chk = -1; static unsigned long long s_calls, s_checks, s_bad, s_cn, s_cl;
    if (__builtin_expect(s_chk < 0, 0)) s_chk = getenv("SPU_NATIVE_CHECK") ? 1 : 0;
    if (s_chk && (__atomic_fetch_add(&s_calls, 1, __ATOMIC_RELAXED) & 1023) == 0) {
        spu_context* c2 = (spu_context*)malloc(sizeof(spu_context));
        if (c2) {
            memcpy(c2, ctx, sizeof(spu_context));
            if (ctx->ls == ctx->ls_store) c2->ls = ctx->ls;   /* the same local store */
            t_busy = 1;
            const unsigned long long tl0 = __rdtsc();
            jobmod_d6cae8b8000a_e11C8_spu_func_00004250(c2);
            __atomic_fetch_add(&s_cl, __rdtsc() - tl0, __ATOMIC_RELAXED);
            __atomic_fetch_add(&s_cn, tn, __ATOMIC_RELAXED);
            t_busy = 0;
            const uint32_t want = c2->gpr[3]._u32[0];
            const unsigned long long k = __atomic_add_fetch(&s_checks, 1, __ATOMIC_RELAXED);
            if (want != mask) {
                const unsigned long long b = __atomic_add_fetch(&s_bad, 1, __ATOMIC_RELAXED);
                if (b <= 8) fprintf(stderr, "[spu-native] dxtblend 4250 MISMATCH: lifted %08X native %08X\n", want, mask);
            }
            if ((k & 0x3FF) == 1) fprintf(stderr, "[spu-native] dxtblend 4250 checks: %llu, mismatches %llu; cycles per call native %llu lifted %llu\n",
                                           k, s_bad, s_cn / k, s_cl / k);
            free(c2);
        }
    }
    ctx->gpr[3]._u32[0] = mask; ctx->gpr[3]._u32[1] = ctx->gpr[3]._u32[2] = ctx->gpr[3]._u32[3] = 0;
    /* SPU_RET: back to the link register */
    ctx->pc = ctx->gpr[0]._u32[0];
    if (ctx->host_depth == 0) g_spu_trampoline_fn = spu_indirect_branch;
    return 1;
}

static int native_4250(spu_context* ctx, uint32_t* out)
{
#if defined(SPU_FP_MX)
    /* SPU_NATIVE_STATS=1: how often it runs and why it falls back, every 2^16 calls */
    static int s_st = -1; static unsigned long long s_n[3];
    if (__builtin_expect(s_st < 0, 0)) s_st = getenv("SPU_NATIVE_STATS") ? 1 : 0;
    if (s_st && (++s_n[0] & 0xFFFF) == 1)
        fprintf(stderr, "[spu-native] dxtblend 4250: %llu calls, %llu not in fast mode, %llu large inputs\n", s_n[0], s_n[1], s_n[2]);
    if (!spu_fp_fast) { if (s_st) s_n[1]++; return 0; }
    const uint32_t pix = ctx->gpr[3]._u32[0] & SPU_LS_MASK & ~15u;
    const u128 A = spu_ls_read128(ctx, ctx->gpr[4]._u32[0]);
    const u128 B = spu_ls_read128(ctx, ctx->gpr[5]._u32[0]);
    float p[3][16];
    uint32_t big = 0;
    for (int k = 0; k < 3; k++) {
        const uint32_t ea = (A._u32[k] >> 23) & 0xFF, eb = (B._u32[k] >> 23) & 0xFF;
        big |= (ea > 250) | (eb > 250);
    }
    for (int i = 0; i < 16; i++) {
        const u128 q = spu_ls_read128(ctx, pix + 16u * (uint32_t)i);
        for (int k = 0; k < 3; k++) {
            big |= ((q._u32[k] >> 23) & 0xFF) > 250;
            p[k][i] = f_of(q._u32[k]);
        }
    }
    if (big) { if (s_st) s_n[2]++; return 0; }
    /* palette, scalar, exactly as the SPU: fs then fma (fused) */
    float c1[3], c2[3], a[3], b[3];
    const __m128 third = _mm_set_ss(f_of(0x3EAAAAAAu)), twothird = _mm_set_ss(f_of(0x3F2AAAAAu));
    for (int k = 0; k < 3; k++) {
        a[k] = f_of(A._u32[k]); b[k] = f_of(B._u32[k]);
        __m128 va = _mm_set_ss(a[k]), vb = _mm_set_ss(b[k]);
        __asm__("" : "+x"(va));
        const __m128 d = _mm_sub_ss(vb, va);
        c1[k] = _mm_cvtss_f32(_mm_fmadd_ss(d, third, va));
        c2[k] = _mm_cvtss_f32(_mm_fmadd_ss(d, twothird, va));
    }
    const __m256 sign = _mm256_castsi256_ps(_mm256_set1_epi32((int)0x80000000u));
    uint32_t mask = 0;
    for (int h = 0; h < 2; h++) {
        const __m256 p0 = _mm256_loadu_ps(&p[0][8 * h]), p1 = _mm256_loadu_ps(&p[1][8 * h]), p2 = _mm256_loadu_ps(&p[2][8 * h]);
#define SUM3(X) ({ __m256 s0 = _mm256_andnot_ps(sign, _mm256_sub_ps(_mm256_set1_ps((X)[0]), p0)); \
                   __m256 s1 = _mm256_andnot_ps(sign, _mm256_sub_ps(_mm256_set1_ps((X)[1]), p1)); \
                   __m256 s2 = _mm256_andnot_ps(sign, _mm256_sub_ps(_mm256_set1_ps((X)[2]), p2)); \
                   __m256 t = _mm256_add_ps(s0, s1); __asm__("" : "+x"(t)); _mm256_add_ps(t, s2); })
        const __m256 sc1 = SUM3(c1), sb = SUM3(b), sa = SUM3(a), sc2 = SUM3(c2);
#undef SUM3
        const __m256i one = _mm256_set1_epi32(1);
        const __m256i b7  = _mm256_and_si256(_mm256_castps_si256(_mm256_cmp_ps(sb,  sc1, _CMP_GT_OQ)), one);
        const __m256i b27 = _mm256_and_si256(_mm256_castps_si256(_mm256_cmp_ps(sa,  sc1, _CMP_GT_OQ)), one);
        const __m256i b29 = _mm256_and_si256(_mm256_castps_si256(_mm256_cmp_ps(sb,  sc2, _CMP_GT_OQ)), one);
        const __m256i b28 = _mm256_and_si256(_mm256_castps_si256(_mm256_cmp_ps(sa,  sc2, _CMP_GT_OQ)), one);
        const __m256i b11 = _mm256_and_si256(_mm256_castps_si256(_mm256_cmp_ps(sc2, sc1, _CMP_GT_OQ)), one);
        const __m256i b10 = _mm256_or_si256(_mm256_and_si256(b28, b29), _mm256_and_si256(b7, b27));
        const __m256i idx = _mm256_or_si256(_mm256_add_epi32(b10, b10), _mm256_and_si256(b7, b11));
        uint32_t v[8]; _mm256_storeu_si256((__m256i*)v, idx);
        for (int j = 0; j < 8; j++) mask |= v[j] << (2 * (8 * h + j));
    }
    *out = mask;
    return 1;
#else
    (void)ctx; (void)out;
    return 0;
#endif
}
