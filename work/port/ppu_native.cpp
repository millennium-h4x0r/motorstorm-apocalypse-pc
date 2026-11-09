/* MotorStorm Apocalypse (BCES-00484): native stand-ins for lifted PPU functions.
 *
 * Registered: the game's event Set (0xFF5B40, see nat_event_set), and two
 * hooks on the game clock that only patch its fields and decline (PS3_SIM_60,
 * see nat_clock_recompute). s_probes is empty; the mechanism is
 * game-independent.
 *
 * The lift calls PPU_NATIVE_HOOK at the entry of each function listed in
 * tools/ppu_native_entries.py (a post-pass over the lifter's output). The
 * hook asks ppu_native_slot, once, for the function registered at its
 * address; a registered function that returns nonzero has done the guest
 * function's work and left the context as the guest's `blr` would, by the
 * PPC64 ELF ABI: r1, r2, r13-r31, f14-f31, v20-v31 and cr2-cr4 as on entry,
 * the result in r3 (f1, v2). Volatile registers are not reproduced: no caller
 * may read them after a call, and the lifted callers do not (their reloads
 * after each call are the registers the localizer found live there). One that
 * returns 0 declines -- a range the inline memory paths would not take, an
 * SPU reservation on a line it would write -- and the lifted body runs.
 *
 * Every function here is a leaf the lifted code runs as a scalar loop over
 * guest memory, with a range test per access. Guest memory is big-endian at
 * vm_base + EA. A native function only takes a call when the whole range it
 * touches is one the inline accessors in ppu_recomp.h would take: the same
 * [0x10000, end of mapping) window, not the gcm control word, and for writes
 * no line an SPU holds a reservation on. Writes are checked again after they
 * land and told through vm_store_coh_notify, as VM_ST_INL does.
 *
 * The same file switches the lift's native blocks (hot loops inside functions
 * too big to replace, patched in by the same post-pass): g_ppu_native_blocks.
 *
 * PS3RECOMP_NATIVE_PPU: 1 (default) on; 0 off (the hooks find nothing, the
 * blocks fall through to the lifted loops); 2 runs the lifted function as well
 * on every call and compares the ABI-visible registers and the bytes the
 * function writes, logging any difference and keeping the lifted result, with
 * a stats line every 10 s (the blocks run as in 1). */
#define PPU_INLINE_STORES 1
#include "ppu_recomp.h"
#include "ppu_native.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <atomic>
#include <utility>
#include <vector>

/* ---- guest memory ranges ------------------------------------------------ */

/* Would every access in [ea, ea+n) take the inline paths? */
static inline int range_ok(uint64_t ea, uint64_t n)
{
    if (__builtin_expect(g_vm_inl_lim == 0, 0)) return 0;
    if (ea - 0x10000u >= g_vm_inl_lim || ea + n > 0x10000ull + g_vm_inl_lim) return 0;
    const uint64_t ctl = ppu_hle_inject_base + 0x2008u;
    return ctl + 4 <= ea || ctl >= ea + n;
}
/* ... and is no line of a write to it reserved by an SPU? */
static inline int range_wr_ok(uint64_t ea, uint64_t n)
{
    if (!range_ok(ea, n)) return 0;
    if (!VM_COH_ARMED() || !n) return 1;
    for (uint64_t l = ea >> 7; l <= (ea + n - 1) >> 7; l++)
        if ((VM_COH_BYTE(l >> 3) >> (l & 7)) & 1u) return 0;
    return 1;
}
/* After the write: a reservation taken while it ran is told, line by line. */
static inline void range_wr_done(uint64_t ea, uint64_t n)
{
    if (!VM_COH_ARMED() || !n) return;
    for (uint64_t l = ea >> 7; l <= (ea + n - 1) >> 7; l++) {
        if (!((VM_COH_BYTE(l >> 3) >> (l & 7)) & 1u)) continue;
        const uint64_t a = l << 7 > ea ? l << 7 : ea, e = (l + 1) << 7 < ea + n ? (l + 1) << 7 : ea + n;
        vm_store_coh_notify((uint32_t)a, (uint32_t)(e - a));
    }
}

/* ---- the functions ------------------------------------------------------ */

/* Example (not registered): a native memcpy(r3 dst, r4 src, r5 n) returning
 * dst. Before registering it for a MotorStorm function, check the guest
 * routine really is this: inFAMOUS 2's copied eight bytes at a time, backward
 * when src's offset in its 4 KB page was below dst's, so overlapping ranges
 * are left to the lifted code here; it zero-extends r3 and r4 and compares r5
 * as 64 bits. To use it: add
 *     { 0x<ADDR>u, "memcpy", nat_memcpy, func_<ADDR>, span_memcpy, PPU_NATIVE_RET_R3 },
 * to s_funcs, declare `extern void func_<ADDR>(ppu_context*);` and list the
 * address in tools/ppu_native_entries.py NATIVE_ENTRIES. */
[[maybe_unused]] static int nat_memcpy(ppu_context* c)
{
    const uint64_t d = (uint32_t)c->gpr[3], s = (uint32_t)c->gpr[4], n = c->gpr[5];
    if (n >> 31 || (d < s + n && s < d + n) || !range_ok(s, n) || !range_wr_ok(d, n)) return 0;
    memcpy(vm_base + d, vm_base + s, n);
    range_wr_done(d, n);
    c->gpr[3] = d;
    return 1;
}
[[maybe_unused]] static int span_memcpy(const ppu_context* c, ppu_native_span* sp)
{
    sp[0].ea = (uint32_t)c->gpr[3];
    sp[0].n = c->gpr[5];
    return 1;
}

/* The game's event, 0xFF5B40: Set(this r3, bool r4), vtable 0x1201500 slot 3.
 *
 * The event is two of the game's spinlocks (func_00675D3C: lwarx/stwcx. of 0
 * to 1, 500 spins, then sys_timer_usleep(250); func_00675E00: lwsync, store
 * 0): +8 guards the event, +12 is the event itself, held while it is reset.
 * Wait (0xFF5AD8) takes and drops +12. Set(true) is lock +8, unlock +12,
 * unlock +8. Set(false), the reset, is lock +8, lock +12, unlock +8 -- and so
 * it spins on +12 *while holding +8* when the event is already reset.
 *
 * Its user is the job batches (0x6737E0 resets a batch's event, then queues
 * it; JobBatchHandler, PPU thread 3, sets it from 0x673BFC when the batch's
 * SPU work is done). Several batches a frame are queued and never waited on;
 * the game counts on each finishing before the next frame queues it again.
 * When one has not -- its completion still on the way when the main thread
 * queues it again -- the main thread spins in the reset on +12 holding +8,
 * and JobBatchHandler, coming to set that same event, spins on +8: neither
 * ever moves (main thread poll site 0xFF5B90, thread 3 at 0xFF5B6C). That
 * froze the end of the logo movie once: AsyncVidRender (thread 40) kept
 * drawing the last picture, and the main thread, which stops it and opens
 * maintitles_60.avi, never came back.
 *
 * So the reset here waits for +12 with +8 let go between tries: the late Set
 * gets in, and the reset then takes the event as it would have had the Set
 * come first. With the event set (every other time), it is the guest's
 * sequence exactly. Set(true) stays lifted. */
static inline int nat_trylock(ppu_context* c, uint32_t ea)
{
    /* lwarx / cmpwi 0 / stwcx. 1, as func_00675D3C does it: a stwcx. that
     * fails with the word still 0 (the reservation was lost) goes round
     * again; only a held lock (nonzero) is a failure */
    for (;;) {
        const uint32_t v = vm_read32(ea);
        PPU_RESV_ADDR(c) = ea;
        c->reserve_value = v;
        ppu_resv_line_note(ea);
        const int ok = v == 0 && ppu_stwcx32(ea, v, 1u);
        PPU_RESV_ADDR(c) = 0;
        if (ok) { PPU_FENCE(acquire); return 1; }
        if (v != 0) return 0;
    }
}
/* The game's spinlock (func_00675D3C / func_00675E00) and its waiters.
 *
 * The guest spins 500 reads and then sleeps 250 us, so every contended
 * acquire costs at least a quarter of a millisecond more than the hold: the
 * main thread waits on the job-batch events (0xFF5AD8) this way several times
 * a frame. Here a waiter spins a little, then sleeps on a futex on the lock
 * word itself, and a native unlock wakes it. The futex wait still times out
 * at 250 us, so an unlock that is not ours (the word written by other guest
 * code) is seen no later than before. Waiters are counted per hashed address
 * so an unlock with nobody waiting makes no system call.
 *
 * PS3_LOCK_NATIVE=0: the lifted lock and unlock (and nat_event_set's own
 * 250 us back-off). PS3_LOCK_SPIN=<n>: spin rounds before sleeping (10). */
static std::atomic<uint32_t> s_lk_waiters[256];
static inline std::atomic<uint32_t>& lk_waiters(uint32_t ea)
{
    return s_lk_waiters[((ea >> 2) * 0x9E3779B1u) >> 24];
}
static int lk_native(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PS3_LOCK_NATIVE"); v = !(e && e[0] == '0'); }
    return v;
}
static int lk_spin_rounds(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PS3_LOCK_SPIN"); v = e ? atoi(e) : 10; if (v < 0) v = 0; }
    return v;
}
/* Wait, up to 250 us, for the word at ea to change from nonzero. */
static void lk_wait(uint32_t ea)
{
    uint32_t* hw = (uint32_t*)(vm_base + ea);
    const int rounds = lk_spin_rounds();
    for (int k = 0; k < rounds; k++) {
        for (int j = 0; j < 8; j++) __builtin_ia32_pause();
        if (__atomic_load_n(hw, __ATOMIC_RELAXED) == 0) return;
    }
    std::atomic<uint32_t>& w = lk_waiters(ea);
    w.fetch_add(1, std::memory_order_seq_cst);
    const uint32_t raw = __atomic_load_n(hw, __ATOMIC_SEQ_CST);
    if (raw != 0) {
        struct timespec ts = {0, 250000};
        syscall(SYS_futex, hw, FUTEX_WAIT_PRIVATE, raw, &ts, nullptr, 0);
    }
    w.fetch_sub(1, std::memory_order_relaxed);
}
static inline void lk_wake(uint32_t ea)
{
    std::atomic_thread_fence(std::memory_order_seq_cst);   /* the store before the waiter count */
    if (lk_waiters(ea).load(std::memory_order_relaxed))
        syscall(SYS_futex, (uint32_t*)(vm_base + ea), FUTEX_WAKE_PRIVATE, INT32_MAX, nullptr, nullptr, 0);
}
static inline void nat_unlock(uint32_t ea)
{
    PPU_FENCE(release);   /* lwsync */
    vm_write32(ea, 0u);
    lk_wake(ea);
}
/* PS3_LOCK_STATS=1: every 10 s, the contended acquires of the game's
 * spinlock -- count and time waited per (guest thread, caller lr) -- so a wait
 * can be named without the 250 us poll lines that no longer exist. */
static uint64_t lk_now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static int lk_stats_on(void)
{
    static int v = -1;
    if (v < 0) v = getenv("PS3_LOCK_STATS") ? 1 : 0;
    return v;
}
extern "C" void ps3_tl(const char* tag, uint32_t a, uint32_t b);
extern "C" int g_ps3_tl;
/* The caller a contended acquire is named by (see lk_stats_note). */
static uint32_t lk_site(const ppu_context* c)
{
    uint32_t site = (uint32_t)c->lr;
    if (site == 0x00FF5AFCu && range_ok(c->gpr[1] + 0x90, 8)) site = (uint32_t)vm_read64(c->gpr[1] + 0x90);
    if (site == 0x00673AE4u && range_ok(c->gpr[1] + 0x80 + 0x80, 8)) site = (uint32_t)vm_read64(c->gpr[1] + 0x80 + 0x80);
    return site;
}
static void lk_stats_note(const ppu_context* c, uint32_t ea, uint64_t ns)
{
    enum { N = 32 };
    static std::atomic<uint64_t> key[N], cnt[N], tot[N];
    static std::atomic<uint32_t> lockea[N];
    /* The event Wait (0xFF5AD8) is one site for every job batch: name its
     * caller instead (Wait saved its lr at r1 + 0x90). */
    uint32_t site = (uint32_t)c->lr;
    if (site == 0x00FF5AFCu && range_ok(c->gpr[1] + 0x90, 8)) site = (uint32_t)vm_read64(c->gpr[1] + 0x90);
    /* ... and through the batch's Wait (0x673AB8, frame 0x70 above Wait's 0x80) */
    if (site == 0x00673AE4u && range_ok(c->gpr[1] + 0x80 + 0x80, 8)) site = (uint32_t)vm_read64(c->gpr[1] + 0x80 + 0x80);
    const uint64_t k = ((uint64_t)(uint32_t)c->thread_id << 32) | site;
    for (unsigned i = (unsigned)(k * 0x9E3779B97F4A7C15ull >> 59), n = 0; n < N; n++, i = (i + 1) % N) {
        uint64_t cur = key[i].load(std::memory_order_relaxed);
        if (cur == 0 && key[i].compare_exchange_strong(cur, k)) cur = k;
        if (cur != k) continue;
        cnt[i].fetch_add(1, std::memory_order_relaxed);
        tot[i].fetch_add(ns, std::memory_order_relaxed);
        lockea[i].store(ea, std::memory_order_relaxed);
        break;
    }
    static std::atomic<uint64_t> next;
    const uint64_t now = lk_now_ns();
    uint64_t t = next.load(std::memory_order_relaxed);
    if (!t) { next.compare_exchange_strong(t, now + 10000000000ull); return; }
    if (now < t || !next.compare_exchange_strong(t, now + 10000000000ull)) return;
    char buf[2048]; int p = snprintf(buf, sizeof buf, "[lock-stats] 10 s, contended acquires (tid lr lock: n, ms):");
    for (unsigned i = 0; i < N; i++) {
        const uint64_t kk = key[i].load(std::memory_order_relaxed), n = cnt[i].exchange(0);
        const uint64_t ms = tot[i].exchange(0) / 1000000ull;
        if (!kk || !n || p > (int)sizeof buf - 64) continue;
        p += snprintf(buf + p, sizeof buf - p, " %u@%08X(%08X):%llu,%llu", (unsigned)(kk >> 32), (uint32_t)kk,
                      lockea[i].load(std::memory_order_relaxed), (unsigned long long)n, (unsigned long long)ms);
    }
    fprintf(stderr, "%s\n", buf);
}

/* func_00675D3C(lock r3): lwarx/stwcx. 0 -> 1, or wait. Leaves r3 = 0 (the
 * word its last lwarx read) and r4 as the guest's spin count would not matter:
 * both are volatile. */
static int nat_lock(ppu_context* c)
{
    if (!lk_native()) return 0;
    const uint32_t ea = (uint32_t)c->gpr[3];
    if (ea < 0x10000u || (ea & 3u)) return 0;
    if (nat_trylock(c, ea)) { c->gpr[3] = 0; return 1; }
    const uint64_t t0 = lk_stats_on() ? lk_now_ns() : 0;
    /* PS3_TIMELINE: a contended acquire's start and end ("lkw0"/"lkw1": thread, caller lr). */
    const int tl = g_ps3_tl;
    if (tl) ps3_tl("lkw0", (uint32_t)c->thread_id, lk_site(c));
    do lk_wait(ea); while (!nat_trylock(c, ea));
    if (tl) ps3_tl("lkw1", (uint32_t)c->thread_id, lk_site(c));
    if (t0) lk_stats_note(c, ea, lk_now_ns() - t0);
    c->gpr[3] = 0;
    return 1;
}
/* func_00675E00(lock r3): lwsync, store 0 -- and wake a waiter. */
static int nat_unlock_fn(ppu_context* c)
{
    if (!lk_native()) return 0;
    const uint32_t ea = (uint32_t)c->gpr[3];
    if (ea < 0x10000u || (ea & 3u)) return 0;
    nat_unlock(ea);
    c->gpr[4] = 0;
    return 1;
}
static void nat_usleep250(ppu_context* c)
{
    /* sys_timer_usleep(250), the guest's own back-off -- through lv2, so
     * PS3_POLLTOP still names the caller (lr) as the poll site */
    const uint64_t r3 = c->gpr[3], r11 = c->gpr[11];
    c->gpr[3] = 250;
    c->gpr[11] = 141;
    lv2_syscall(c);
    c->gpr[3] = r3;
    c->gpr[11] = r11;
}
static int nat_event_set(ppu_context* c)
{
    if ((uint8_t)c->gpr[4]) return 0;               /* Set(true): lifted */
    /* The out-of-line accessors (vm_read32/vm_write32/ppu_stwcx32) take any
     * guest address and tell SPU reservations themselves, so no range test:
     * the job batches' events live at 0x4A8xxxxx, where range_wr_ok declined. */
    const uint32_t obj = (uint32_t)c->gpr[3];
    if (obj < 0x10000u) return 0;
    const uint32_t guard = obj + 8, ev = obj + 12;
    uint32_t tries = 0;
    const int nl = lk_native();
    for (;;) {
        while (!nat_trylock(c, guard)) { if (nl) lk_wait(guard); else nat_usleep250(c); }
        const int got = nat_trylock(c, ev);
        nat_unlock(guard);
        if (got) break;
        tries++;
        if (nl) lk_wait(ev); else nat_usleep250(c);
    }
    if (tries) {
        /* the reset the lifted code would have deadlocked in */
        static std::atomic<int> told;
        if (told.fetch_add(1, std::memory_order_relaxed) < 8)
            fprintf(stderr, "[ppu-native] event 0x%08X reset while still reset (caller 0x%08X): "
                    "waited %u x 250 us for its Set\n", obj, (uint32_t)c->lr, tries);
    }
    c->gpr[3] = guard;    /* what the guest's last call (unlock +8) leaves */
    return 1;
}

/* Simulation pacing, PS3_SIM_60 (default on; 0 = the game as shipped). See
 * docs/NOTES.md "Simulation pacing".
 *
 * The game advances its simulation by a clock source object (constructor
 * 0x5EE2A4, from 0x2D6C90): +0x00 int64 step in us, +0x10 refresh Hz, +0x14
 * float scale, +0x18 vsync interval, +0x1C real-time flag, +0x20/+0x21 two
 * slack flags. Every setter (0x5EE3B4 Hz, 0x5EE3D8 +0x20, 0x5EE3FC scale,
 * 0x5EE420 interval, 0x5EE444 +0x21; the real-time flag is a plain store,
 * 0x4F8F60) ends in 0x5EE178, which recomputes the step from them. The
 * frame timer (0x5EE620) adds step to the game time each frame when the flag
 * is 0, and (now - last) * scale when it is 1. The game's setup (0x2D6F30,
 * again 0x2D6E58) asks for 60 Hz, interval 2, flag 0: a fixed 1/30 s a frame,
 * with the present (0x6AF3E8, interval from [[cfg+0x18]+0x18] = this +0x18)
 * flipping every second vblank. At less than 30 fps the race runs slow.
 *
 * With PS3_SIM_60 the game clock is kept real-time with interval 1: before
 * every recompute, flag 1 and an interval above 1 made 1. Scale still
 * applies, so the crash slow motion (scale 0.35, 0.107) is as it was. Only the
 * instance the constructor built: a second clock (0x5EE310, from 0x1D5680, on
 * the stack, 30 Hz fixed) is left alone. Both hooks decline, so the lifted
 * bodies run as before, on the patched fields. */
static int sim60_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PS3_SIM_60"); v = !e || atoi(e) != 0; }
    return v;
}
/* Two constants of the race's dt path, rewritten once in guest memory (the
 * lift reads both from there), after checking they are the game's:
 *
 * The clamp. func_001DA398 clamps a frame's dt to 0.04 s (float compare
 * 0x1DA384, double result 0x1DA388): under 25 fps the race ran slower than
 * wall time, and in the 25-30 fps the port reaches, every frame over 40 ms
 * lost time (0.88x at 25.5 fps). PS3_SIM_DT_MAX_MS=<ms> (20..200) sets it;
 * the default is 66.7 ms (1/15 s): real time down to 15 fps. Havok gets one
 * step a frame with this dt either way (its task runs per frame do not change
 * with dt); 6 minutes of race at 100 ms steps showed no trouble. 40 is the
 * game's.
 *
 * The snap. func_00EC8A98 replaces a dt in 0.031..0.035 s by exactly 1/30 s
 * (doubles 0xEC8A80 / 0xEC8A88): right for a game locked to 30 Hz, a 5% error
 * per frame for a real-time one near 30 fps. Turned off by raising the low
 * bound to 1e30 (the test is "dt <= low: no snap"). PS3_SIM_SNAP=1 keeps it. */
static void sim_patch_consts(void)
{
    double ms = 1000.0 / 15.0;
    const char* e = getenv("PS3_SIM_DT_MAX_MS");
    if (e && *e) {
        const double v = atof(e);
        if (v >= 20.0 && v <= 200.0) ms = v;
        else fprintf(stderr, "[sim60] PS3_SIM_DT_MAX_MS=%s out of range (20..200), using %.1f\n", e, ms);
    }
    if (vm_read32(0x1DA384u) == 0x3D23D70Au && vm_read64(0x1DA388u) == 0x3FA47AE140000000ull) {
        const float f = (float)(ms / 1000.0);
        const double d = (double)f;
        uint32_t fw; uint64_t dw;
        memcpy(&fw, &f, 4); memcpy(&dw, &d, 8);
        vm_write32(0x1DA384u, fw);
        vm_write64(0x1DA388u, dw);
        fprintf(stderr, "[sim60] frame dt clamp %.1f ms (the game's: 40)\n", ms);
    } else {
        fprintf(stderr, "[sim60] the dt clamp at 0x1DA384 is not the one expected: left alone\n");
    }
    const char* sn = getenv("PS3_SIM_SNAP");
    if (sn && atoi(sn) != 0) return;
    if (vm_read64(0xEC8A80u) == 0x3F9FBE76C8B43958ull && vm_read64(0xEC8A88u) == 0x3FA1EB851EB851ECull) {
        const double big = 1e30;
        uint64_t bw; memcpy(&bw, &big, 8);
        vm_write64(0xEC8A80u, bw);
        fprintf(stderr, "[sim60] 1/30 s dt snap off\n");
    } else {
        fprintf(stderr, "[sim60] the dt snap at 0xEC8A80 is not the one expected: left alone\n");
    }
}
static std::atomic<uint32_t> s_sim_clock;
static int nat_clock_ctor(ppu_context* c)      /* 0x5EE2A4(this r3) */
{
    if (sim60_on()) {
        static std::atomic<int> once;
        if (!once.exchange(1)) sim_patch_consts();
        const uint32_t src = (uint32_t)c->gpr[3];
        if (s_sim_clock.exchange(src) != src)
            fprintf(stderr, "[sim60] game clock source at 0x%08X: real-time, vsync interval 1\n", src);
    }
    return 0;
}
static int nat_clock_recompute(ppu_context* c) /* 0x5EE178(this r3) */
{
    const uint32_t src = (uint32_t)c->gpr[3];
    if (!src || src != s_sim_clock.load(std::memory_order_relaxed)) return 0;
    const uint32_t iv = vm_read32(src + 0x18), rt = vm_read32(src + 0x1C);
    if (rt == 1 && iv <= 1) return 0;
    vm_write32(src + 0x1C, 1u);
    if (iv > 1) vm_write32(src + 0x18, 1u);
    static std::atomic<int> told;
    if (told.fetch_add(1, std::memory_order_relaxed) < 16)
        fprintf(stderr, "[sim60] clock 0x%08X set to %u Hz, interval %u, %s (caller 0x%08X): "
                "made real-time, interval 1\n", src, vm_read32(src + 0x10), iv, rt ? "real-time" : "fixed step",
                (uint32_t)c->lr);
    return 0;
}

/* Add MotorStorm entries here (and their addresses to
 * tools/ppu_native_entries.py NATIVE_ENTRIES). The last row is an end marker
 * that keeps the array non-empty; the registry skips it (no fn). */
/* The race's dt clamp, func_001DA398(this r3, dt f1 in seconds): counts the
 * frame time it takes and the part the clamp (0x1DA384) cuts off, so a test
 * (tests/sim_speed.sh reads these two symbols from /proc/<pid>/mem) can tell a
 * slow race clock from frames the clamp shortened. Always declines. */
extern "C" { volatile uint64_t g_sim_dt_us, g_sim_clamped_us; }
static int nat_dt_clamp_note(ppu_context* c)
{
    const double dt = c->fpr[1];
    if (!(dt > 0.0 && dt < 10.0)) return 0;
    const uint32_t w = vm_read32(0x1DA384u);
    float clamp; memcpy(&clamp, &w, 4);
    __atomic_fetch_add(&g_sim_dt_us, (uint64_t)(dt * 1e6), __ATOMIC_RELAXED);
    if (dt > (double)clamp)
        __atomic_fetch_add(&g_sim_clamped_us, (uint64_t)((dt - (double)clamp) * 1e6), __ATOMIC_RELAXED);
    return 0;
}

/* func_00911128's packet lists (tools/ppu_native_entries.py NATIVE_BLOCKS,
 * the block at loc_00911378, the top of its per-packet loop).
 *
 * The function walks lists of packets: r19 is the cursor (first at list+16),
 * the list ends at [r1+0xEB8] (list + size word + 16) and each packet's byte
 * at +3 is its size in bytes, a multiple of 16; the copy loops at 0x9113F8
 * and 0x911480 move (byte >> 4) quadwords two at a time and stop only for a
 * count of 1..15. A header whose count is 0 makes that copy run forever, 32
 * bytes a pass, up through the flat guest window until the first page that
 * is not writable -- the guard under the first guest thread stack, which is
 * why every such crash stored to exactly 0xD0000000 (and why everything from
 * the list's target up to there, VRAM included, was trashed first). On a PS3
 * that header would be just as fatal: the list was read before whoever
 * writes it had finished (or after a lost update).
 *
 * So: a cursor at a packet with a count of 0, or past the end, ends the list
 * there (the block sets r19 to the end and takes the function's own
 * end-of-list path). Logged with the list, the bytes and the guest call
 * chain, once per distinct (cursor, end) for the first 16; then counted.
 * PS3_PKT_GUARD=0 turns it off (the crash comes back); PS3_PKT_GUARD=2 also
 * calls spu_coh_dump() on the first one (SPU_LINE_WATCH ring and every SPU
 * context's reservation state). */
extern "C" void spu_coh_dump(void);
extern "C" int ppu_pkt_guard(uint32_t p, uint32_t end, uint32_t r1)
{
    static int s_mode = -1;
    if (s_mode < 0) { const char* e = getenv("PS3_PKT_GUARD"); s_mode = e ? atoi(e) : 1; }
    if (!s_mode) return 0;
    if (p < end && p >= 0x10000u && (uint64_t)p + 4 <= 0x100000000ull &&
        (vm_read8(p + 3) >> 4) != 0)
        return 0;                                   /* the normal case */
    static std::atomic<uint32_t> s_hits{0};
    static uint32_t s_seen[16][2];
    const uint32_t n = s_hits.fetch_add(1) + 1;
    int fresh = 1;
    for (int i = 0; i < 16 && i < (int)n - 1; i++)
        if (s_seen[i][0] == p && s_seen[i][1] == end) fresh = 0;
    if (fresh && n <= 16) {
        s_seen[n - 1][0] = p; s_seen[n - 1][1] = end;
        fprintf(stderr, "[pkt-guard] func_00911128: %s at 0x%08X (list end 0x%08X): list cut there "
                "(hit %u)\n", p >= end ? "cursor past the end" : "packet with a quadword count of 0",
                p, end, n);
        if (p >= 0x10000u && p < 0xFFFFFFC0u) {
            fprintf(stderr, "[pkt-guard]   bytes at 0x%08X:", p & ~0xFu);
            for (uint32_t i = 0; i < 64; i++)
                fprintf(stderr, "%s%02X", (i % 16) ? " " : "\n[pkt-guard]     ", vm_read8((p & ~0xFu) + i));
            fprintf(stderr, "\n");
        }
        /* the guest call chain: back chain at 0(r1), saved LR at +16 */
        fprintf(stderr, "[pkt-guard]   called from:");
        uint32_t sp = r1;
        for (int i = 0; i < 12 && sp >= 0x10000u && sp < 0xFFFFFF00u; i++) {
            const uint32_t next = (uint32_t)vm_read64(sp);
            if (next <= sp) break;
            fprintf(stderr, " 0x%08X", (uint32_t)vm_read64(next + 16));
            sp = next;
        }
        fprintf(stderr, "\n");
        if (s_mode >= 2 && n == 1) spu_coh_dump();
    } else if ((n & (n - 1)) == 0) {
        fprintf(stderr, "[pkt-guard] func_00911128: %u lists cut so far\n", n);
    }
    return 1;
}

/* PS3_HAVOK_TRACE=1: a ring of the Havok job-queue traffic -- PPU addJob
 * (0x8BDB08), the PPU semaphore acquire (0x8C20D8) and its sibling 0x8C219C,
 * the raycast batch wait (0x7820C0) and build (0x7828D0), plus SPU task
 * creates and returns (havok_trace_note from libspurs / spu_workload) --
 * dumped by the taskset watchdog (SPU_TS_WATCHDOG) at a stall. All hooks
 * decline. Diagnostic. */
struct hv_ev { uint64_t t; uint32_t tid, kind, a, b, c, d, e, f, q[4]; };
/* The job queue header's words +0x14 +0x18 +0x20 +0x2C (line 0x14700F80), at each event. */
static inline void hv_q(hv_ev& e) { static const uint32_t o[4] = { 0x14, 0x18, 0x20, 0x2C };
    for (int i = 0; i < 4; i++) e.q[i] = vm_read32(0x14700F80u + o[i]); }
static hv_ev s_hv[8192]; static std::atomic<uint32_t> s_hv_pos;
static int hv_on(void) { static int v = -1; if (v < 0) { const char* e = getenv("PS3_HAVOK_TRACE"); v = e && e[0] == '1'; } return v; }
extern "C" void havok_trace_note(uint32_t kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    if (!hv_on()) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    hv_ev& e = s_hv[s_hv_pos.fetch_add(1, std::memory_order_relaxed) & 8191u];
    e.t = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
    e.tid = (uint32_t)syscall(SYS_gettid); e.kind = kind; e.a = a; e.b = b; e.c = c; e.d = d; e.e = 0; e.f = 0; hv_q(e);
}
static void hv_ppu(ppu_context* c, uint32_t kind, uint32_t a, uint32_t b, uint32_t cc, uint32_t d, uint32_t ee = 0, uint32_t f = 0)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    hv_ev& e = s_hv[s_hv_pos.fetch_add(1, std::memory_order_relaxed) & 8191u];
    e.t = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
    e.tid = (uint32_t)c->thread_id | 0x80000000u; e.kind = kind; e.a = a; e.b = b; e.c = cc; e.d = d; e.e = ee; e.f = f; hv_q(e);
    (void)c;
}
static int nat_hv_addjob(ppu_context* c)      /* 0x8BDB08(queue r3, job r4, r5) */
{
    if (!hv_on()) return 0;
    const uint32_t j = (uint32_t)c->gpr[4];
    hv_ppu(c, 1, (uint32_t)c->gpr[3], j, (uint32_t)c->gpr[5], vm_read32(j), vm_read32(j + 4), (uint32_t)c->lr);
    /* the raycast (type 3) job's whole 120 bytes, the first 64 times */
    if ((vm_read32(j) >> 16) == 3u) { static std::atomic<int> n;
        if (n.fetch_add(1) < 64) { fprintf(stderr, "[hv-job]");
            for (uint32_t o = 0; o < 120; o += 4) fprintf(stderr, " %08X", vm_read32(j + o));
            fprintf(stderr, "\n"); } }
    return 0;
}
static int nat_hv_semacq(ppu_context* c)      /* 0x8C20D8(sem r3): word at [r3+4] */
{
    if (!hv_on()) return 0;
    const uint32_t s = (uint32_t)c->gpr[3], w = vm_read32(s + 4);
    hv_ppu(c, 2, s, w, vm_read32(w), vm_read32(s + 0xC), (uint32_t)c->lr);
    return 0;
}
static int nat_hv_sem2(ppu_context* c)        /* 0x8C219C(sem r3) */
{
    if (!hv_on()) return 0;
    const uint32_t s = (uint32_t)c->gpr[3], w = vm_read32(s + 4);
    hv_ppu(c, 3, s, w, vm_read32(w), (uint32_t)c->gpr[4], (uint32_t)c->lr);
    return 0;
}
static int nat_hv_wait(ppu_context* c)        /* 0x7820C0(batch r3): waits on [[r3+0x10]] if [r3+0xC] */
{
    if (!hv_on()) return 0;
    const uint32_t o = (uint32_t)c->gpr[3], sp = vm_read32(o + 0x10);
    hv_ppu(c, 4, o, vm_read32(o + 0xC), sp, sp ? vm_read32(vm_read32(sp + 4)) : 0, vm_read32(o + 4), (uint32_t)c->lr);
    return 0;
}
static int nat_hv_build(ppu_context* c)       /* 0x7828D0(r3, r4, r5) */
{
    if (!hv_on()) return 0;
    hv_ppu(c, 5, (uint32_t)c->gpr[3], (uint32_t)c->gpr[4], (uint32_t)c->gpr[5], (uint32_t)c->gpr[6], (uint32_t)c->lr);
    return 0;
}
extern "C" void havok_trace_dump(void)
{
    if (!hv_on()) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
    static const char* nm[] = { "?", "addJob", "semAcq", "sem2", "batchWait", "batchBuild", "?", "?", "?", "?",
                                "spuRet", "create", "spuStart", "?" };
    const uint32_t end = s_hv_pos.load(), k0 = end > 8192 ? end - 8192 : 0;
    for (uint32_t k = k0; k < end; k++) {
        const hv_ev& e = s_hv[k & 8191u];
        fprintf(stderr, "[havok-trace] -%.4f %s %s%u a=%08X b=%08X c=%08X d=%08X e=%08X f=%08X q=%X/%X/%X/%X\n", (now - e.t) / 1e6,
                nm[e.kind < 14 ? e.kind : 13], e.tid & 0x80000000u ? "ppu" : "host", e.tid & 0x7FFFFFFFu,
                e.a, e.b, e.c, e.d, e.e, e.f, e.q[0], e.q[1], e.q[2], e.q[3]);
    }
}

static const ppu_native_desc s_funcs[] = {
    /* PS3_HAVOK_TRACE: job-queue traffic observers; all decline */
    { 0x008BDB08u, "hv_addjob", nat_hv_addjob, nullptr, nullptr, 0u },
    { 0x008C20D8u, "hv_semacq", nat_hv_semacq, nullptr, nullptr, 0u },
    { 0x008C219Cu, "hv_sem2", nat_hv_sem2, nullptr, nullptr, 0u },
    { 0x007820C0u, "hv_wait", nat_hv_wait, nullptr, nullptr, 0u },
    { 0x007828D0u, "hv_build", nat_hv_build, nullptr, nullptr, 0u },
    /* no compare (lifted = null): the lifted reset is the deadlock */
    { 0x00FF5B40u, "event_set", nat_event_set, nullptr, nullptr, 0u },
    /* the game's spinlock: futex waits instead of 250 us sleeps */
    { 0x00675D3Cu, "spin_lock", nat_lock, nullptr, nullptr, 0u },
    { 0x00675E00u, "spin_unlock", nat_unlock_fn, nullptr, nullptr, 0u },
    /* PS3_SIM_60: both always decline after patching the clock's fields */
    { 0x005EE2A4u, "clock_ctor", nat_clock_ctor, nullptr, nullptr, 0u },
    { 0x005EE178u, "clock_recompute", nat_clock_recompute, nullptr, nullptr, 0u },
    /* counts frame time and the time the dt clamp cuts; declines */
    { 0x001DA398u, "dt_clamp_note", nat_dt_clamp_note, nullptr, nullptr, 0u },
    { 0u, nullptr, nullptr, nullptr, nullptr, 0u },
};

/* ---- registry ----------------------------------------------------------- */

#define PPU_NATIVE_MAX 32
static ppu_native_desc s_tab[PPU_NATIVE_MAX];
static int s_n;
static int s_mode = -1;
/* The lift's native blocks (tools/ppu_native_entries.py NATIVE_BLOCKS) run
 * while this is set. */
extern "C" { int g_ppu_native_blocks = 1; }

extern "C" int ppu_native_register_desc(const ppu_native_desc* d)
{
    if (s_n >= PPU_NATIVE_MAX || !d->fn) return -1;
    s_tab[s_n++] = *d;
    return 0;
}
extern "C" int ppu_native_register(uint32_t addr, int (*fn)(ppu_context*))
{
    ppu_native_desc d = {};
    d.addr = addr; d.name = "?"; d.fn = fn;
    return ppu_native_register_desc(&d);
}
extern "C" const ppu_native_desc* ppu_native_table(int* n)
{
    *n = s_n;
    return s_tab;
}
/* ---- call-count probes -------------------------------------------------- */

/* Functions listed in tools/ppu_native_entries.py PROBES get a hook whose
 * native side runs the lifted body and times it: calls a second, time a
 * second inside and the longest call, every 10 s on stderr ([ppu-probe]).
 * For finding which game loops run how often and how long. Enabled with
 * PS3RECOMP_PPU_PROBE=1. */
struct ppu_probe { uint32_t addr; void (*body)(ppu_context*); };
/* Add MotorStorm probes here, keeping in step with PROBES in
 * tools/ppu_native_entries.py, e.g. (example address):
 *     extern void func_00123456(ppu_context*);
 *     ... { 0x00123456u, func_00123456 },
 * The last row is an end marker that keeps the array non-empty. */
static const ppu_probe s_probes[] = {
    { 0u, nullptr },
};
#define N_PROBES (sizeof s_probes / sizeof s_probes[0] - 1)
/* sized +1 so an empty table still declares them */
static std::atomic<uint64_t> s_probe_n[N_PROBES + 1], s_probe_ns[N_PROBES + 1], s_probe_max[N_PROBES + 1];
[[maybe_unused]] static __thread uint8_t t_probe_in[N_PROBES + 1];
[[maybe_unused]] static uint64_t probe_now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
[[maybe_unused]] static void probe_tick(void)
{
    static std::atomic<uint64_t> next;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = (uint64_t)ts.tv_sec;
    uint64_t t = next.load(std::memory_order_relaxed);
    if (!t) { next.compare_exchange_strong(t, now + 10); return; }
    if (now < t || !next.compare_exchange_strong(t, now + 10)) return;
    /* per function: calls/s, ms/s spent inside, the longest call in us */
    fprintf(stderr, "[ppu-probe] calls/s ms/s max-us:");
    for (size_t i = 0; i < N_PROBES; i++)
        fprintf(stderr, " %08X=%llu/%.1f/%llu", s_probes[i].addr,
                (unsigned long long)(s_probe_n[i].exchange(0, std::memory_order_relaxed) / 10),
                (double)s_probe_ns[i].exchange(0, std::memory_order_relaxed) / 1e7,
                (unsigned long long)(s_probe_max[i].exchange(0, std::memory_order_relaxed) / 1000));
    fputc('\n', stderr);
}
/* A probe runs the lifted body itself to time it (its own hook declines while
 * it does, so a recursive call is timed as part of the outermost one and not
 * counted). Game-specific result accounting for one probe goes after the
 * timing, keyed on s_probes[I].addr. */
template <size_t I> static int probe_fn(ppu_context* c)
{
    if (t_probe_in[I]) return 0;   /* the inner run, or a recursive call */
    if (I == 0) probe_tick();
    t_probe_in[I] = 1;
    const uint64_t t0 = probe_now_ns();
    s_probes[I].body(c);
    const uint64_t dt = probe_now_ns() - t0;
    t_probe_in[I] = 0;
    s_probe_n[I].fetch_add(1, std::memory_order_relaxed);
    s_probe_ns[I].fetch_add(dt, std::memory_order_relaxed);
    uint64_t m = s_probe_max[I].load(std::memory_order_relaxed);
    while (dt > m && !s_probe_max[I].compare_exchange_weak(m, dt, std::memory_order_relaxed)) { }
    return 1;
}
template <size_t... I> static void probe_register(std::index_sequence<I...>)
{
    ((void)ppu_native_register(s_probes[I].addr, probe_fn<I>), ...);
}

__attribute__((constructor)) static void ppu_native_init(void)
{
    for (const ppu_native_desc& d : s_funcs)
        if (d.fn) ppu_native_register_desc(&d);
    if (getenv("PS3RECOMP_PPU_PROBE")) probe_register(std::make_index_sequence<N_PROBES>{});
    const char* e = getenv("PS3RECOMP_NATIVE_PPU");
    g_ppu_native_blocks = !e || atoi(e) != 0;
}

/* ---- compare mode ------------------------------------------------------- */

static __thread int t_in_compare;
static std::atomic<uint64_t> s_cmp_calls[PPU_NATIVE_MAX], s_cmp_diff[PPU_NATIVE_MAX],
    s_cmp_declined[PPU_NATIVE_MAX], s_cmp_skipped[PPU_NATIVE_MAX];

static int abi_diff(const ppu_context* a, const ppu_context* b, uint32_t ret, char* why, size_t wn)
{
    static const int gprs[] = {1, 2, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
    for (int r : gprs)
        if (a->gpr[r] != b->gpr[r]) { snprintf(why, wn, "r%d lifted %llx native %llx", r, (unsigned long long)a->gpr[r], (unsigned long long)b->gpr[r]); return 1; }
    if ((ret & PPU_NATIVE_RET_R3) && a->gpr[3] != b->gpr[3]) {
        snprintf(why, wn, "r3 lifted %llx native %llx", (unsigned long long)a->gpr[3], (unsigned long long)b->gpr[3]);
        return 1;
    }
    if ((a->cr ^ b->cr) & 0x00FFF000u) { snprintf(why, wn, "cr2-4 lifted %08x native %08x", a->cr, b->cr); return 1; }
    for (int r = 14; r < 32; r++)
        if (memcmp(&a->fpr[r], &b->fpr[r], 8)) { snprintf(why, wn, "f%d", r); return 1; }
    for (int r = 20; r < 32; r++)
        if (memcmp(&a->vr[r], &b->vr[r], 16)) { snprintf(why, wn, "v%d", r); return 1; }
    if ((ret & PPU_NATIVE_RET_F1) && memcmp(&a->fpr[1], &b->fpr[1], 8)) { snprintf(why, wn, "f1"); return 1; }
    if ((ret & PPU_NATIVE_RET_V2) && memcmp(&a->vr[2], &b->vr[2], 16)) { snprintf(why, wn, "v2"); return 1; }
    return 0;
}

static void cmp_stats(void)
{
    static std::atomic<time_t> next;
    const time_t now = time(nullptr);
    time_t t = next.load(std::memory_order_relaxed);
    if (now < t || !next.compare_exchange_strong(t, now + 10, std::memory_order_relaxed)) return;
    if (!t) return;
    fprintf(stderr, "[ppu-native] compare:");
    for (int i = 0; i < s_n; i++)
        fprintf(stderr, " %s %llu (differ %llu, declined %llu, too big %llu)", s_tab[i].name,
                (unsigned long long)s_cmp_calls[i].load(), (unsigned long long)s_cmp_diff[i].load(),
                (unsigned long long)s_cmp_declined[i].load(), (unsigned long long)s_cmp_skipped[i].load());
    fprintf(stderr, "\n");
}

/* Both, on the same entry state: the lifted function first (whose result is
 * kept), then, on the bytes as they were, the native one. */
static int compare_run(int i, ppu_context* ctx)
{
    if (t_in_compare) return 0;     /* the lifted body below, through its own hook */
    const ppu_native_desc& d = s_tab[i];
    if (!d.lifted) return d.fn(ctx);
    cmp_stats();
    ppu_native_span sp[PPU_NATIVE_SPANS];
    const int ns = d.writes ? d.writes(ctx, sp) : 0;
    uint64_t total = 0;
    for (int k = 0; k < ns; k++) {
        if (!range_ok(sp[k].ea, sp[k].n)) return 0;
        total += sp[k].n;
    }
    if (total > (1u << 22)) { s_cmp_skipped[i]++; return 0; }
    static __thread std::vector<uint8_t>* pre, *post;
    if (!pre) { pre = new std::vector<uint8_t>; post = new std::vector<uint8_t>; }
    pre->resize(total); post->resize(total);
    uint64_t o = 0;
    for (int k = 0; k < ns; o += sp[k].n, k++) memcpy(pre->data() + o, vm_base + sp[k].ea, sp[k].n);
    const ppu_context in = *ctx;
    t_in_compare = 1;
    d.lifted(ctx);
    t_in_compare = 0;
    o = 0;
    for (int k = 0; k < ns; o += sp[k].n, k++) {
        memcpy(post->data() + o, vm_base + sp[k].ea, sp[k].n);
        memcpy(vm_base + sp[k].ea, pre->data() + o, sp[k].n);
    }
    ppu_context nat = in;
    const int took = d.fn(&nat);
    char why[160] = "";
    int bad = 0;
    if (took) {
        bad = abi_diff(ctx, &nat, d.ret, why, sizeof why);
        o = 0;
        for (int k = 0; k < ns && !bad; o += sp[k].n, k++)
            if (memcmp(post->data() + o, vm_base + sp[k].ea, sp[k].n)) {
                uint64_t j = 0;
                while (post->data()[o + j] == vm_base[sp[k].ea + j]) j++;
                snprintf(why, sizeof why, "byte at %08llx lifted %02x native %02x", (unsigned long long)(sp[k].ea + j),
                         post->data()[o + j], vm_base[sp[k].ea + j]);
                bad = 1;
            }
    }
    o = 0;
    for (int k = 0; k < ns; o += sp[k].n, k++) memcpy(vm_base + sp[k].ea, post->data() + o, sp[k].n);
    s_cmp_calls[i]++;
    if (!took) s_cmp_declined[i]++;
    if (bad && s_cmp_diff[i]++ < 20)
        fprintf(stderr, "[ppu-native] %s (0x%08X) differs: %s; r3 %llx r4 %llx r5 %llx r6 %llx lr %llx\n", d.name, d.addr,
                why, (unsigned long long)in.gpr[3], (unsigned long long)in.gpr[4], (unsigned long long)in.gpr[5],
                (unsigned long long)in.gpr[6], (unsigned long long)in.lr);
    return 1;
}
template <int I> static int compare_thunk(ppu_context* ctx) { return compare_run(I, ctx); }
template <int... I> struct thunks { static constexpr int (*tab[])(ppu_context*) = {compare_thunk<I>...}; };
static constexpr auto& s_thunks = thunks<0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
                                          21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31>::tab;
static_assert(sizeof s_thunks / sizeof s_thunks[0] == PPU_NATIVE_MAX, "one compare thunk per slot");

/* ---- the hook's lookup -------------------------------------------------- */

extern "C" int (*ppu_native_slot(uint32_t addr))(ppu_context*)
{
    static std::atomic<int> once;
    if (s_mode < 0) {
        const char* e = getenv("PS3RECOMP_NATIVE_PPU");
        const int m = e ? atoi(e) : 1;
        int z = 0;
        if (once.compare_exchange_strong(z, 1))
            fprintf(stderr, "[ppu-native] %d native PPU function(s): %s\n", s_n,
                    m == 2 ? "compared with the lifted ones" : m ? "on" : "off");
        s_mode = m;
    }
    /* PS3RECOMP_NATIVE_PPU_OFF=<hex>,...: those entries stay lifted (A/B of one) */
    if (const char* off = getenv("PS3RECOMP_NATIVE_PPU_OFF"))
        for (const char* p = off; *p; ) {
            char* end;
            const unsigned long a = strtoul(p, &end, 16);
            if (end == p) break;
            if (a == addr) return nullptr;
            p = *end == ',' ? end + 1 : end;
        }
    for (int i = 0; i < s_n; i++)
        if (s_tab[i].addr == addr) return s_mode == 0 ? nullptr : s_mode == 2 ? s_thunks[i] : s_tab[i].fn;
    return nullptr;
}
