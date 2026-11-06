/*
 * test_vdec_real -- cellVdec / cellAdec against one of MotorStorm
 * Apocalypse's real movies.
 *
 * A minimal AVI reader walks the 'movi' list and feeds every 00dc chunk
 * (XviD VOP) to cellVdecDecodeAu and every 01wb chunk (one AC3 frame) to
 * cellAdecDecodeAu, the way the title's own MoviePlayer demuxer does: through
 * guest memory, with the guest structs big-endian at their SDK offsets, and
 * drained with GetPicItem/GetPicture and GetPcmItem/GetPcm in the formats
 * the title asks for (YUV420 planar; AC3 as big-endian float stereo with
 * channelPointers {0,2,1,3,4,5}).
 *
 * Writes:
 *   out/frame_NNNN.png        YUV420 from GetPicture, converted here (BT.601)
 *   out/frame_NNNN_argb.png   the same picture through GetPicture ARGB32/BT.709
 *   out/audio.wav             the decoded PCM (s16 stereo 48 kHz)
 *   out/audio_ref.f32         (made by run_test.sh with the ffmpeg CLI, for
 *                             the correlation check)
 *
 * Build/run: ./run_test.sh  (needs the ps3/ symlink tree next to this file)
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

uint8_t* vm_base;
uint32_t ppu_hle_inject_base = 0x00F00000u;
#include "ps3/libs/codec/cellVdec.c"
#include "ps3/libs/codec/cellAdec.c"
#include "ps3/libs/codec/vdec_ffmpeg.c"
#include "ps3/libs/codec/ffmpeg_runtime.c"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "ps3/libs/codec/stb_image_write.h"

PPU_THREAD_LOCAL ppu_context* g_active_ctx;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
unsigned char g_spu_coh_bitmap[1 << 22];
int g_spu_coh_armed;
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_coh_notify_write_from_ppu(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
uint64_t spu_lockline_mask_range(uint32_t ea, uint32_t len) { (void)ea; (void)len; return 0; }
void spu_lockline_lock_mask(uint64_t m) { (void)m; }
void spu_lockline_unlock_mask(uint64_t m) { (void)m; }
ps3_guest_caller_fn g_ps3_guest_caller;

/* Guest memory map of the test. */
enum {
    MEM = 64u << 20,
    VTYPE = 0x1000, VSPEC = 0x1040, ATTR = 0x1100, VCB = 0x1200, VHOUT = 0x1300,
    VAU = 0x1400, FMT = 0x1500, ITEMP = 0x1600,
    ATYPE = 0x2000, ACB = 0x2100, AHOUT = 0x2200, AAU = 0x2300, AITEMP = 0x2400,
    APARAM = 0x2500,
    AUDATA = 0x01000000, PIC = 0x01400000, PCMOUT = 0x01800000,
};

static unsigned n_vau, n_picout, n_aau, n_pcmout;
static void guest_cb(u32 opd, u64 h, u64 type, u64 data, u64 arg,
                     u64 a, u64 b, u64 c, u64 d)
{
    (void)h; (void)data; (void)arg; (void)a; (void)b; (void)c; (void)d;
    if (opd == 0xC0DE) {                 /* vdec */
        if (type == CELL_VDEC_MSG_TYPE_AUDONE) n_vau++;
        if (type == CELL_VDEC_MSG_TYPE_PICOUT) n_picout++;
    } else if (opd == 0xADEC) {
        if (type == CELL_ADEC_MSG_TYPE_AUDONE) n_aau++;
        if (type == CELL_ADEC_MSG_TYPE_PCMOUT) n_pcmout++;
    }
}

static u8 clamp8(double v) { return v < 0 ? 0 : v > 255 ? 255 : (u8)(v + 0.5); }

/* Guest YUV420 planar -> RGB PNG (BT.601 limited range). */
static void save_yuv_png(const char* path, u32 ea, u32 w, u32 h)
{
    const u8* Y = vm_base + ea;
    const u8* U = Y + w * h;
    const u8* V = U + (w / 2) * (h / 2);
    u8* rgb = malloc((size_t)w * h * 3);
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            const double yy = 1.164 * (Y[y * w + x] - 16);
            const double u = U[(y / 2) * (w / 2) + x / 2] - 128;
            const double v = V[(y / 2) * (w / 2) + x / 2] - 128;
            u8* p = rgb + (y * w + x) * 3;
            p[0] = clamp8(yy + 1.596 * v);
            p[1] = clamp8(yy - 0.392 * u - 0.813 * v);
            p[2] = clamp8(yy + 2.017 * u);
        }
    stbi_write_png(path, (int)w, (int)h, 3, rgb, (int)w * 3);
    free(rgb);
}

/* Guest ARGB32 -> PNG. */
static void save_argb_png(const char* path, u32 ea, u32 w, u32 h, u8* alpha_out)
{
    const u8* s = vm_base + ea;
    u8* rgb = malloc((size_t)w * h * 3);
    for (u32 i = 0; i < w * h; i++) {
        rgb[i * 3 + 0] = s[i * 4 + 1];
        rgb[i * 3 + 1] = s[i * 4 + 2];
        rgb[i * 3 + 2] = s[i * 4 + 3];
    }
    *alpha_out = s[0];
    stbi_write_png(path, (int)w, (int)h, 3, rgb, (int)w * 3);
    free(rgb);
}

static u32 rd32le(const u8* p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

static void wr_wav(const char* path, const s16* pcm, u32 frames)
{
    FILE* f = fopen(path, "wb");
    u32 data = frames * 4;
    u8 h[44] = "RIFF....WAVEfmt ";
    #define W32(o, v) do { h[o] = (u8)(v); h[o+1] = (u8)((v) >> 8); h[o+2] = (u8)((v) >> 16); h[o+3] = (u8)((v) >> 24); } while (0)
    W32(4, 36 + data); W32(16, 16); h[20] = 1; h[22] = 2; W32(24, 48000); W32(28, 48000 * 4);
    h[32] = 4; h[34] = 16; memcpy(h + 36, "data", 4); W32(40, data);
    fwrite(h, 1, 44, f); fwrite(pcm, 4, frames, f); fclose(f);
}

int main(int argc, char** argv)
{
    const char* avi = argc > 1 ? argv[1] : "evologo_60.avi";
    const char* outdir = argc > 2 ? argv[2] : "out";
    const unsigned max_vframes = argc > 3 ? (unsigned)atoi(argv[3]) : 300;
    char path[512];

    FILE* f = fopen(avi, "rb");
    if (!f) { perror(avi); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    u8* file = malloc((size_t)fsz);
    if (fread(file, 1, (size_t)fsz, f) != (size_t)fsz) return 1;
    fclose(f);

    vm_base = calloc(1, MEM);
    g_ps3_guest_caller = guest_cb;

    /* ---- video decoder, as the title opens it ---- */
    vm_write32(VTYPE + 0, CELL_VDEC_CODEC_TYPE_DIVX);
    vm_write32(VTYPE + 4, 0);
    vm_write32(VTYPE + 8, VSPEC);
    vm_write32(VSPEC + 0, 8);              /* CellVdecDivxSpecificInfo */
    vm_write16(VSPEC + 4, 1280);
    vm_write16(VSPEC + 6, 720);
    assert(cellVdecQueryAttrEx((void*)(uintptr_t)VTYPE, (void*)(uintptr_t)ATTR) == CELL_OK);
    vm_write32(VCB + 0, 0xC0DE); vm_write32(VCB + 4, 0x1111);
    assert(cellVdecOpenEx((void*)(uintptr_t)VTYPE, NULL, (void*)(uintptr_t)VCB,
                          (CellVdecHandle*)(uintptr_t)VHOUT) == CELL_OK);
    const CellVdecHandle vh = vm_read32(VHOUT);
    assert(cellVdecStartSeq(vh) == CELL_OK);
    assert(cellVdecSetFrameRate(vh, 0x84) == CELL_OK);

    /* ---- AC3 decoder, CellAdecParamAc3 exactly as fn 0x9EE6D0 fills it ---- */
    vm_write32(ATYPE, CELL_ADEC_TYPE_AC3);
    vm_write32(ACB + 0, 0xADEC); vm_write32(ACB + 4, 0x2222);
    assert(cellAdecOpenEx((void*)(uintptr_t)ATYPE, NULL, (void*)(uintptr_t)ACB,
                          (CellAdecHandle*)(uintptr_t)AHOUT) == CELL_OK);
    const CellAdecHandle ah = vm_read32(AHOUT);
    /* AC3CH=6: the title's 5.1 request (3_2 + LFE, channelPointers 0..5). */
    const u32 nch = getenv("AC3CH") && atoi(getenv("AC3CH")) == 6 ? 6 : 2;
    vm_write8 (APARAM + 0x00, 1);          /* wordSize FLOAT */
    vm_write8 (APARAM + 0x01, nch == 6 ? 7 : 2);   /* outputMode 3_2 / 2_0 */
    vm_write8 (APARAM + 0x02, nch == 6);   /* outLfeOn */
    vm_write32(APARAM + 0x04, 0x3F800000); /* drcCut 1.0 */
    vm_write32(APARAM + 0x08, 0x3F800000); /* drcBoost 1.0 */
    vm_write8 (APARAM + 0x0C, 2);          /* compression LINE_OUT */
    vm_write8 (APARAM + 0x0D, 2);
    vm_write32(APARAM + 0x14, 0x3F800000); /* pcmScaleFactor 1.0 */
    const u32 cp2[6] = { 0, 2, 1, 3, 4, 5 }, cp6[6] = { 0, 1, 2, 3, 4, 5 };
    for (int i = 0; i < 6; i++) vm_write32(APARAM + 0x18 + i * 4, nch == 6 ? cp6[i] : cp2[i]);
    assert(cellAdecStartSeq(ah, (void*)(uintptr_t)APARAM) == CELL_OK);

    /* ---- walk movi ---- */
    const u8* movi = NULL;
    for (long i = 12; i + 12 < fsz; ) {
        const u32 sz = rd32le(file + i + 4);
        if (!memcmp(file + i, "LIST", 4) && !memcmp(file + i + 8, "movi", 4)) { movi = file + i + 12; fsz = i + 8 + sz; break; }
        i += 8 + sz + (sz & 1);
    }
    assert(movi);
    const u8* end = file + fsz;

    const unsigned want[] = { 0, 15, 45, 90, 150, 220, 290 };
    unsigned vframe = 0, saved = 0;
    float* pcm = malloc(sizeof(float) * 6 * 48000 * 12);
    u32 pcm_frames = 0, pcm_items = 0;
    const u32 max_pcm = 48000 * 12;
    vm_write32(FMT + 0, 3); vm_write32(FMT + 4, 0); vm_write8(FMT + 8, 0xFF);

    for (const u8* p = movi; p + 8 <= end && vframe < max_vframes; ) {
        const u32 sz = rd32le(p + 4);
        if (!memcmp(p, "LIST", 4)) { p += 12; continue; }        /* rec lists */
        const u8* data = p + 8;
        const u32 padded = sz + (sz & 1);   /* the title passes the padded size */
        if (!memcmp(p + 2, "dc", 2) && sz) {
            memcpy(vm_base + AUDATA, data, sz);
            if (padded > sz) vm_base[AUDATA + sz] = 0;
            vm_write32(VAU + 0x00, AUDATA);
            vm_write32(VAU + 0x04, padded);
            vm_write64(VAU + 0x08, ~0ull);
            vm_write64(VAU + 0x10, ~0ull);
            vm_write64(VAU + 0x18, vframe);
            assert(cellVdecDecodeAu(vh, 0, (void*)(uintptr_t)VAU) == CELL_OK);
            while (cellVdecGetPicItem(vh, (const CellVdecPicItem**)(uintptr_t)ITEMP) == CELL_OK) {
                const u32 it = vm_read32(ITEMP);
                assert(vm_read32(it + 0x00) == CELL_VDEC_CODEC_TYPE_DIVX);
                assert(vm_read32(it + 0x04) == AUDATA);
                assert(vm_read32(it + 0x40) == 0 && vm_read32(it + 0x44) == 0);
                const u32 info = vm_read32(it + 0x48);
                const u32 w = vm_read16(info + 2), h = vm_read16(info + 4);
                assert(w == 1280 && h == 720);
                assert(vm_read16(info + 0xE) == 5);   /* DIVX_FRC_30 */
                const int is_want = saved < sizeof want / sizeof want[0] && vframe == want[saved];
                if (is_want) {
                    printf("frame %u: pictureType %u, AU size %u\n", vframe, vm_read8(info), padded);
                    /* the same picture twice: once as ARGB (peek, then put
                     * it back by copying the queue head), once as YUV420. */
                    VdecSlot* v = &s_vdec[VDEC_SLOT_OF(vh)];
                    VdecPic keep = v->queue[v->qHead];
                    keep.yuv = (u8*)malloc((size_t)w * h * 3 / 2);
                    memcpy(keep.yuv, v->queue[v->qHead].yuv, (size_t)w * h * 3 / 2);
                    vm_write32(FMT + 0, 0); vm_write32(FMT + 4, 1); vm_write8(FMT + 8, 0x80);
                    assert(cellVdecGetPicture(vh, (void*)(uintptr_t)FMT, (void*)(uintptr_t)PIC) == CELL_OK);
                    u8 a;
                    snprintf(path, sizeof path, "%s/frame_%04u_argb.png", outdir, vframe);
                    save_argb_png(path, PIC, w, h, &a);
                    assert(a == 0x80);
                    /* requeue */
                    v->qHead = (v->qHead + VDEC_QUEUE - 1) % VDEC_QUEUE; v->qCount++;
                    v->queue[v->qHead] = keep;
                    vm_write32(FMT + 0, 3); vm_write32(FMT + 4, 0); vm_write8(FMT + 8, 0xFF);
                }
                assert(cellVdecGetPicture(vh, (void*)(uintptr_t)FMT, (void*)(uintptr_t)PIC) == CELL_OK);
                if (is_want) {
                    snprintf(path, sizeof path, "%s/frame_%04u.png", outdir, vframe);
                    save_yuv_png(path, PIC, w, h);
                    /* luma statistics: black would be all 16 */
                    double sum = 0, sq = 0;
                    for (u32 i = 0; i < w * h; i++) { const double y = vm_base[PIC + i]; sum += y; sq += y * y; }
                    const double mean = sum / (w * h);
                    printf("  Y mean %.1f, stddev %.1f -> %s\n", mean, sqrt(sq / (w * h) - mean * mean), path);
                    saved++;
                }
                vframe++;
            }
        } else if (!memcmp(p, "01wb", 4) && sz) {   /* first audio stream only */
            memcpy(vm_base + AUDATA + 0x200000, data, sz);
            vm_write32(AAU + 0x00, AUDATA + 0x200000);
            vm_write32(AAU + 0x04, sz);
            vm_write32(AAU + 0x08, 0xFFFFFFFFu); vm_write32(AAU + 0x0C, 0xFFFFFFFFu);
            vm_write64(AAU + 0x10, 0);
            assert(cellAdecDecodeAu(ah, (void*)(uintptr_t)AAU) == CELL_OK);
            while (cellAdecGetPcmItem(ah, (const CellAdecPcmItem**)(uintptr_t)AITEMP) == CELL_OK) {
                const u32 it = vm_read32(AITEMP);
                const u32 size = vm_read32(it + 0x0C);
                const u32 start = vm_read32(it + 0x08);
                const u32 bsi = vm_read32(it + 0x10);
                assert(size == 1536 * nch * 4);
                assert(vm_read32(bsi + 0x14) == size && vm_read8(bsi + 0x25) == nch);
                assert(vm_read32(it + 0x18) == AUDATA + 0x200000 && vm_read32(it + 0x1C) == sz);
                assert(cellAdecGetPcm(ah, (void*)(uintptr_t)PCMOUT) == CELL_OK);
                assert(!memcmp(vm_base + start, vm_base + PCMOUT, size));
                for (u32 i = 0; i < size / 4 && pcm_frames < max_pcm; i += nch) {
                    for (u32 c = 0; c < nch; c++)
                        pcm[pcm_frames * nch + c] = vm_read_f32(PCMOUT + (i + c) * 4);
                    pcm_frames++;
                }
                pcm_items++;
            }
        }
        p += 8 + padded;
    }
    printf("video: %u AUs -> %u AUDONE, %u PICOUT, %u pictures taken\n", n_vau, n_vau, n_picout, vframe);
    assert(n_vau == n_picout && n_picout == vframe);
    printf("audio: %u AUs -> %u PCMOUT, %u items, %u sample frames (%.2f s)\n",
           n_aau, n_pcmout, pcm_items, pcm_frames, pcm_frames / 48000.0);
    assert(n_aau == n_pcmout && n_pcmout == pcm_items);

    /* PCM statistics over the whole decoded stretch and per 0.5 s. */
    double peak = 0, sq[6] = { 0 };
    for (u32 i = 0; i < pcm_frames; i++)
        for (u32 c = 0; c < nch; c++) {
            const double s = pcm[i * nch + c];
            sq[c] += s * s; if (fabs(s) > peak) peak = fabs(s);
        }
    printf("  peak %.3f, RMS per output channel:", peak);
    for (u32 c = 0; c < nch; c++) printf(" %.4f", sqrt(sq[c] / pcm_frames));
    printf("\n");
    for (u32 b = 0; b + 24000 <= pcm_frames; b += 24000) {
        double s2 = 0; unsigned zc = 0;
        for (u32 i = b; i < b + 24000; i++) {
            s2 += pcm[i * nch] * pcm[i * nch];
            if (i > b && (pcm[i * nch] >= 0) != (pcm[(i - 1) * nch] >= 0)) zc++;
        }
        printf("  t=%4.1fs  RMS %.4f  zero-crossings/s %u\n", b / 48000.0, sqrt(s2 / 24000), zc * 2);
    }

    /* Against the ffmpeg CLI's own decode of the same stream, if present. */
    snprintf(path, sizeof path, "%s/audio_ref.f32", outdir);
    FILE* rf = fopen(path, "rb");
    if (rf) {
        float* ref = malloc(sizeof(float) * nch * pcm_frames);
        const size_t nref = fread(ref, sizeof(float) * nch, pcm_frames, rf);
        fclose(rf);
        /* our slot order L C R Ls Rs LFE vs ffmpeg's FL FR FC LFE SL SR */
        static const int map6[6] = { 0, 2, 1, 4, 5, 3 }, map2[2] = { 0, 1 };
        const int* map = nch == 6 ? map6 : map2;
        double xy = 0, xx = 0, yy = 0, err = 0;
        for (size_t i = 0; i < nref; i++)
            for (u32 c = 0; c < nch; c++) {
                const double x = pcm[i * nch + c], y = ref[i * nch + map[c]];
                xy += x * y; xx += x * x; yy += y * y; err += (x - y) * (x - y);
            }
        printf("  vs ffmpeg CLI decode (%zu frames): correlation %.6f, SNR %.1f dB\n",
               nref, xy / sqrt(xx * yy + 1e-30), 10 * log10(yy / (err + 1e-30)));
        free(ref);
    }

    s16* s = malloc(sizeof(s16) * 2 * pcm_frames);
    for (u32 i = 0; i < pcm_frames * 2; i++) {      /* front L/R for the wav */
        const float x = pcm[(i / 2) * nch + (i & 1 ? (nch == 6 ? 2 : 1) : 0)];
        double v = x * 32767.0; s[i] = (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    snprintf(path, sizeof path, "%s/audio.wav", outdir);
    wr_wav(path, s, pcm_frames);

    assert(cellVdecEndSeq(vh) == CELL_OK && cellVdecClose(vh) == CELL_OK);
    assert(cellAdecEndSeq(ah) == CELL_OK && cellAdecClose(ah) == CELL_OK);
    puts("test_vdec_real: ok");
    return 0;
}
