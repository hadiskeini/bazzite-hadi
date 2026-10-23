// vkpace: Vulkan layer that paces a game at its VK_AMD_anti_lag input-stage call (Deadlock with
// r_low_latency 1 calls it on the main thread before sampling input). Absolute 141-slot grid with
// catch-up, the driver's own anti-lag kept underneath (guarded after stalls), optional flip-locked
// scheduling. Always loaded; passive until PACE_FPS is set. Logs to $VKPACE_LOG when set. README.md.
// Log lines: P/p present entry/exit, Q present id, F flip timing, W swapchain, A/a anti-lag
// entry/exit, S pacing record, D device, C pacer config, G proc-addr lookup. Single-device,
// single-swapchain assumption.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <stdio.h>
#include <stdarg.h>

static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_vkGetDeviceProcAddr next_gdpa;
static PFN_vkQueuePresentKHR next_present;
static PFN_vkCreateDevice next_create_device;
static PFN_vkDestroyDevice next_destroy_device;
static PFN_vkAntiLagUpdateAMD next_antilag;
static PFN_vkCreateSwapchainKHR next_create_swapchain;
static PFN_vkDestroySwapchainKHR next_destroy_swapchain;
static PFN_vkSetSwapchainPresentTimingQueueSizeEXT next_set_queue_size;
static PFN_vkGetSwapchainTimeDomainPropertiesEXT next_time_domains;
static PFN_vkGetPastPresentationTimingEXT next_past_timing;
static VkInstance g_instance; static VkDevice g_device;

/* ---- logging ---- */
/* Logging is off unless VKPACE_LOG is set. Lines are buffered and written synchronously every
 * 100 lines; a background writer thread proved unreliable inside the Wine process (it never ran,
 * and all but the first two buffers were lost), so the simple scheme stays. */
static int fd = -1, log_on; static char buf[1 << 16]; static int blen; static int nlines;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static void flush_buf(void){ if (fd >= 0 && blen > 0) { ssize_t r = write(fd, buf, blen); (void)r; } blen = 0; }
static void put(const char *s){ int n = strlen(s); if (blen + n > (int)sizeof buf) flush_buf(); memcpy(buf + blen, s, n); blen += n; }
static void logline_fn(const char *fmt, ...){ char line[512]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    pthread_mutex_lock(&mu); put(line); if (++nlines % 100 == 0) flush_buf(); pthread_mutex_unlock(&mu); }
static void log_shutdown(void){ pthread_mutex_lock(&mu); flush_buf(); pthread_mutex_unlock(&mu); }
#define logline(...) do { if (log_on) logline_fn(__VA_ARGS__); } while (0)
static int64_t now_ns(void){ struct timespec m; clock_gettime(CLOCK_MONOTONIC, &m); return (int64_t)m.tv_sec * 1000000000LL + m.tv_nsec; }
static long tid(void){ static __thread long t; if (!t) t = (long)syscall(SYS_gettid); return t; }
static void open_log(void);

/* ---- pacer ---- */
static int64_t T;                      /* period in ns; 0 = pacing off */
static int mode_period;                /* 1 = relative period like the engine/driver limiter, 0 = absolute grid */
static double catchup = 1.0;           /* grid: rebase when more than this many periods late */
static int64_t spin_ns = 200000, spin_fixed;     /* spin window; adapts to measured timer lateness unless PACE_SPIN_US is set */
static int64_t late_ema = 60000;
static int driver_on = 1;              /* forward mode ON so the driver's GPU-bound anti-lag stays active */
static pthread_mutex_t pmu = PTHREAD_MUTEX_INITIALIZER;
static int64_t origin, last_exit; static uint64_t f0; static int grid_valid; static int rebases;
#define RING 64
/* tick predictor: the engine tick (PACE_TICK_HZ) makes the first frame that starts after it heavy.
 * The tick phase is tracked as a circular mean of the heavy frames' start windows; a frame whose
 * slot is predicted to follow a tick starts up to tick_adv earlier so that it presents on time. */
static double tick_period = 1e9 / 64;  /* ns; Source 2 ticks at 64 Hz; PACE_TICK_HZ=0 turns the predictor off */
static int64_t tick_adv = 0; static double heavy_thresh;   /* ns */
static double ph_sin, ph_cos; static int64_t prev_start, prev_exit; static int prev_pred;
/* just-in-time input scheduling against the panel's flip grid (PACE_JIT=1, needs PACE_TICK_HZ):
 * a frame predicted light starts later so that it presents just before its flip slot instead of
 * waiting in the queue with stale input. Heavy frames keep their grid slot. */
static int jit_on; static int64_t jit_margin = 500000, jit_max = 1500000, g_ema = 5500000;
/* driver guard: the driver's anti-lag keeps a timing history that a pause, a menu (game mode OFF)
 * poisons, which shows as seconds of stutter afterwards. After any such event
 * the driver is forwarded mode OFF for driver_cooldown and only then gets control back. */
static int64_t driver_cooldown = 2000000000, driver_hold_until, last_input_t; static int last_mode = -1, last_rebases;
static int64_t c_light_ema, w_light_ema;        /* input exit -> present call, and main-thread work, light frames */
static int64_t tflip_ema = 6945000, flip_anchor_t; static uint64_t flip_anchor_id;
#define PRING 256
static struct { uint64_t id; int64_t present_t, flip_t; } pring[PRING];      /* by present id */
static struct { uint64_t frame; int64_t in_exit; int pred; uint64_t id; } fring[RING];   /* by frame */
static uint64_t rt_frame;                        /* frame of the last present-stage call on the render thread */

static void pace_init(void){ const char *e;
    if ((e = getenv("PACE_FPS")) && atof(e) > 0) T = (int64_t)(1e9 / atof(e));
    if ((e = getenv("PACE_MODE")) && !strcmp(e, "period")) mode_period = 1;
    if ((e = getenv("PACE_CATCHUP"))) catchup = atof(e);
    if ((e = getenv("PACE_SPIN_US"))) { spin_ns = atoll(e) * 1000; spin_fixed = 1; }
    if ((e = getenv("PACE_DRIVER")) && !strcmp(e, "off")) driver_on = 0;
    if ((e = getenv("PACE_TICK_HZ"))) tick_period = atof(e) > 0 ? 1e9 / atof(e) : 0;
    if ((e = getenv("PACE_TICK_ADVANCE_MS"))) tick_adv = (int64_t)(atof(e) * 1e6);
    heavy_thresh = 0.93 * (double)T; if ((e = getenv("PACE_TICK_HEAVY_MS"))) heavy_thresh = atof(e) * 1e6;
    if ((e = getenv("PACE_JIT"))) jit_on = atoi(e) && tick_period > 0;
    if ((e = getenv("PACE_JIT_MARGIN_MS"))) jit_margin = (int64_t)(atof(e) * 1e6);
    if ((e = getenv("PACE_JIT_MAX_MS"))) jit_max = (int64_t)(atof(e) * 1e6);
    if ((e = getenv("PACE_JIT_GPU_MS"))) g_ema = (int64_t)(atof(e) * 1e6);
    if ((e = getenv("PACE_DRIVER_COOLDOWN_MS"))) driver_cooldown = (int64_t)(atof(e) * 1e6);
    logline("C T=%lld mode=%s catchup=%.2f spin_us=%lld driver=%d tick_period_ns=%.0f tick_adv_ns=%lld heavy_ns=%.0f jit=%d jit_margin=%lld jit_max=%lld driver_cooldown=%lld\n",
        (long long)T, mode_period ? "period" : "grid", catchup, (long long)spin_ns / 1000, driver_on, tick_period, (long long)tick_adv, heavy_thresh, jit_on, (long long)jit_margin, (long long)jit_max, (long long)driver_cooldown); }

/* Sleep on the kernel timer until spin_ns before the target, then spin on the clock. The timer wakes
 * late by a jittery amount, so the spin window tracks the measured lateness (about 3x its running
 * mean) and the CPU burnt on spinning stays at what the precision needs. */
static void sleep_until(int64_t target){
    static __thread int slack_set; if (!slack_set) { prctl(PR_SET_TIMERSLACK, 1000); slack_set = 1; }
    int64_t t = target - spin_ns;
    if (t > now_ns()) { struct timespec ts = { t / 1000000000LL, t % 1000000000LL }; while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
        int64_t late = now_ns() - t; if (late < 0) late = 0; if (late > 2000000) late = 2000000;
        if (!spin_fixed) { late_ema = (late_ema * 15 + late) / 16; int64_t w = late_ema * 3 + 20000; if (w < 50000) w = 50000; if (w > 400000) w = 400000; spin_ns = w; } }
    while (now_ns() < target) __builtin_ia32_pause(); }

/* Returns 1 when a tick is predicted between the previous frame start and this slot. Also learns
 * the phase from the frame that just ended (heavy = its main-thread work exceeded heavy_thresh). */
static int tick_due(int64_t slot){
    if (ph_sin == 0 && ph_cos == 0) return 0;
    double phase = atan2(ph_sin, ph_cos) / (2 * M_PI) * tick_period; if (phase < 0) phase += tick_period;
    double since = fmod((double)slot - phase, tick_period); if (since < 0) since += tick_period;
    return since < (double)T; }
static int tick_predict(int64_t t_in, int64_t slot, int *learned_heavy){
    *learned_heavy = 0;
    if (prev_exit && prev_start) {
        int64_t work = t_in - prev_exit;
        if ((double)work > heavy_thresh) { *learned_heavy = 1;
            double mid = (double)(prev_start + (prev_exit - prev_start) / 2) - (double)T / 2;   /* tick lies in (start of the frame before, start of this one] */
            double ang = 2 * M_PI * fmod(mid, tick_period) / tick_period;
            ph_sin = 0.9 * ph_sin + 0.1 * sin(ang); ph_cos = 0.9 * ph_cos + 0.1 * cos(ang); } }
    return tick_due(slot); }

/* Input stage: the slot for frame f is origin + (f - f0) * T. A late frame gets no sleep and the
 * following frames sleep less until the grid is reached again (catch-up); more than `catchup`
 * periods late rebases the grid so the game never runs several frames back to back. */
static void pace(int stage, uint64_t frame, int64_t t_in, int64_t t_fwd){
    if (stage == VK_ANTI_LAG_STAGE_INPUT_AMD) {
        int64_t slot, target; int pred = 0, heavy = 0;
        pthread_mutex_lock(&pmu);
        if (mode_period) slot = last_exit ? last_exit + T : t_fwd;
        else {
            if (!grid_valid || frame < f0) { origin = t_fwd; f0 = frame; grid_valid = 1; }
            slot = origin + (int64_t)(frame - f0) * T;
            if ((double)(t_fwd - slot) > catchup * (double)T) { origin = t_fwd - (int64_t)(frame - f0) * T; slot = t_fwd; rebases++; } }
        target = slot; int64_t extra = 0;
        if (tick_period > 0) {
            pred = tick_predict(t_in, slot, &heavy);
            if (prev_exit && !prev_pred && !heavy) { int64_t w = t_in - prev_exit; w_light_ema = w_light_ema ? (w_light_ema * 7 + w) / 8 : w; }
            if (pred) target = slot - tick_adv;
            else if (jit_on && flip_anchor_t && c_light_ema && !tick_due(slot + T)) {
                /* flip slot of this frame = anchor flip + (frames since anchor) * flip interval */
                /* the main thread runs ahead: take the newest frame that already has a present id */
                uint64_t id_next = 0, bf = 0, bid = 0;
                for (int i = 0; i < RING; i++) if (fring[i].id && fring[i].frame < frame && fring[i].frame >= bf) { bf = fring[i].frame; bid = fring[i].id; }
                if (bid) id_next = bid + (frame - bf);
                if (id_next > flip_anchor_id) {
                    int64_t slot_flip = flip_anchor_t + (int64_t)(id_next - flip_anchor_id) * tflip_ema;
                    int64_t t_jit = slot_flip - c_light_ema - g_ema - jit_margin;
                    int64_t room = T - w_light_ema - jit_margin; if (room > jit_max) room = jit_max;
                    extra = t_jit - slot; if (extra < 0) extra = 0; if (extra > room) extra = room > 0 ? room : 0;
                    target = slot + extra; } } }
        pthread_mutex_unlock(&pmu);
        if (target > t_fwd) sleep_until(target);
        int64_t t_out = now_ns(); last_exit = t_out; prev_start = t_out; prev_exit = t_out; prev_pred = pred;
        pthread_mutex_lock(&pmu); fring[frame % RING].frame = frame; fring[frame % RING].in_exit = t_out; fring[frame % RING].pred = pred; fring[frame % RING].id = 0; pthread_mutex_unlock(&pmu);
        logline("S %lld %lld %lld %ld %lld %llu %d %d %d %lld %lld\n", (long long)t_in, (long long)t_fwd, (long long)t_out, tid(), (long long)slot, (unsigned long long)frame, rebases, pred, heavy, (long long)extra, (long long)spin_ns);
    } else if (stage == VK_ANTI_LAG_STAGE_PRESENT_AMD) {
        /* remember the frame the render thread is about to present, and learn the light-frame CPU path */
        pthread_mutex_lock(&pmu); rt_frame = frame;
        if (fring[frame % RING].frame == frame && !fring[frame % RING].pred) { int64_t c = t_fwd - fring[frame % RING].in_exit; if (c > 0 && c < 50000000) c_light_ema = c_light_ema ? (c_light_ema * 7 + c) / 8 : c; }
        pthread_mutex_unlock(&pmu); } }

static void open_log(void){ if (fd >= 0 || log_on) return; const char *p = getenv("VKPACE_LOG");
    if (p && *p) { fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644); if (fd >= 0) log_on = 1; }
    struct timespec r; clock_gettime(CLOCK_REALTIME, &r);
    logline("# start realtime_ns=%lld monotonic_ns=%lld\n", (long long)r.tv_sec * 1000000000LL + r.tv_nsec, (long long)now_ns()); pace_init(); }

/* ---- present timing ---- */
static int want_timing = 1, timing_ok; static VkSwapchainKHR g_swapchain; static uint64_t domain_id; static int domain_kind = -1;
static VkPresentStageFlagsEXT stage_mask = VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT | VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT | VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT | VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT;
static uint64_t present_counter;

static void harvest_timing(void){
    VkPastPresentationTimingInfoEXT info = { .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT, .flags = VK_PAST_PRESENTATION_TIMING_ALLOW_PARTIAL_RESULTS_BIT_EXT | VK_PAST_PRESENTATION_TIMING_ALLOW_OUT_OF_ORDER_RESULTS_BIT_EXT, .swapchain = g_swapchain };
    VkPastPresentationTimingPropertiesEXT props = { .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT };
    if (next_past_timing(g_device, &info, &props) < 0 || props.presentationTimingCount == 0) return;
    enum { MAXR = 64, MAXS = 4 }; static VkPastPresentationTimingEXT res[MAXR]; static VkPresentStageTimeEXT st[MAXR][MAXS];
    uint32_t n = props.presentationTimingCount; if (n > MAXR) n = MAXR;
    for (uint32_t i = 0; i < n; i++) { memset(&res[i], 0, sizeof res[i]); res[i].sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT; res[i].presentStageCount = MAXS; res[i].pPresentStages = st[i]; }
    props.presentationTimingCount = n; props.pPresentationTimings = res;
    if (next_past_timing(g_device, &info, &props) < 0) return;
    for (uint32_t i = 0; i < props.presentationTimingCount; i++) {
        for (uint32_t s = 0; s < res[i].presentStageCount; s++) if (res[i].pPresentStages[s].stage == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT && res[i].pPresentStages[s].time) {
            uint64_t id = res[i].presentId; int64_t ft = (int64_t)res[i].pPresentStages[s].time;
            pthread_mutex_lock(&pmu);
            if (pring[id % PRING].id == id) { pring[id % PRING].flip_t = ft;
                if (pring[(id - 1) % PRING].id == id - 1 && pring[(id - 1) % PRING].flip_t) {
                    int64_t dt = ft - pring[(id - 1) % PRING].flip_t; if (dt > 3000000 && dt < 20000000) tflip_ema = (tflip_ema * 7 + dt) / 8;
                    int64_t x = pring[id % PRING].present_t - pring[(id - 1) % PRING].flip_t, y = ft - pring[id % PRING].present_t;
                    if (x >= 1000000 && y > 1000000 && y < 20000000) g_ema = (g_ema * 7 + y) / 8; }
                if (id > flip_anchor_id) { flip_anchor_id = id; flip_anchor_t = ft; } }
            pthread_mutex_unlock(&pmu); }
        if (!log_on) continue;
        char line[256]; int o = snprintf(line, sizeof line, "F %llu %d %d", (unsigned long long)res[i].presentId, (int)res[i].timeDomain, (int)res[i].reportComplete);
        for (uint32_t s = 0; s < res[i].presentStageCount && o < (int)sizeof line - 40; s++) o += snprintf(line + o, sizeof line - o, " %u:%llu", res[i].pPresentStages[s].stage, (unsigned long long)res[i].pPresentStages[s].time);
        snprintf(line + o, sizeof line - o, "\n"); logline("%s", line); } }

static VKAPI_ATTR VkResult VKAPI_CALL vkpace_CreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *al, VkSwapchainKHR *sc){
    VkResult r = VK_ERROR_INITIALIZATION_FAILED; timing_ok = 0;
    if (want_timing && !(ci->flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT) && next_set_queue_size && next_past_timing && next_time_domains) {
        VkSwapchainCreateInfoKHR c = *ci; c.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT | VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
        r = next_create_swapchain(dev, &c, al, sc);
        if (r == VK_SUCCESS) {
            timing_ok = 1; g_swapchain = *sc;
            next_set_queue_size(dev, *sc, 128);
            uint64_t counter = 0; VkSwapchainTimeDomainPropertiesEXT td = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT };
            if (next_time_domains(dev, *sc, &td, &counter) >= 0 && td.timeDomainCount) {
                VkTimeDomainKHR kinds[16]; uint64_t ids[16]; if (td.timeDomainCount > 16) td.timeDomainCount = 16; td.pTimeDomains = kinds; td.pTimeDomainIds = ids;
                if (next_time_domains(dev, *sc, &td, &counter) >= 0) { domain_id = ids[0]; domain_kind = kinds[0];
                    for (uint32_t i = 0; i < td.timeDomainCount; i++) { logline("T domain kind=%d id=%llu\n", (int)kinds[i], (unsigned long long)ids[i]); if (kinds[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR) { domain_id = ids[i]; domain_kind = kinds[i]; } } } } } }
    if (r != VK_SUCCESS) r = next_create_swapchain(dev, ci, al, sc);
    logline("W %lld timing=%d mode=%d images=%u %ux%u flags=0x%x domain=%d\n", (long long)now_ns(), timing_ok, (int)ci->presentMode, ci->minImageCount, ci->imageExtent.width, ci->imageExtent.height, ci->flags, domain_kind);
    return r; }
static VKAPI_ATTR void VKAPI_CALL vkpace_DestroySwapchainKHR(VkDevice dev, VkSwapchainKHR sc, const VkAllocationCallbacks *al){
    if (sc == g_swapchain) { timing_ok = 0; g_swapchain = VK_NULL_HANDLE; } next_destroy_swapchain(dev, sc, al); }

/* ---- hooks ---- */
static VKAPI_ATTR VkResult VKAPI_CALL vkpace_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR *info){
    int64_t t0 = now_ns(); VkResult r;
    if (timing_ok && info->swapchainCount == 1 && info->pSwapchains[0] == g_swapchain) {
        uint64_t id = ++present_counter; int has_id = 0;
        pthread_mutex_lock(&pmu); pring[id % PRING].id = id; pring[id % PRING].present_t = t0; pring[id % PRING].flip_t = 0;
        if (fring[rt_frame % RING].frame == rt_frame) fring[rt_frame % RING].id = id; pthread_mutex_unlock(&pmu);
        int has_timing = 0;
        for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext) { if (p->sType == VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR || p->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR) has_id = 1; if (p->sType == VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT) has_timing = 1; }
        if (has_timing) { present_counter--; logline("P %lld %ld\n", (long long)t0, tid()); r = next_present(q, info); logline("p %lld %ld\n", (long long)now_ns(), tid()); return r; }
        VkPresentId2KHR pid = { .sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR, .pNext = info->pNext, .swapchainCount = 1, .pPresentIds = &id };
        VkPresentTimingInfoEXT ti = { .sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT, .timeDomainId = domain_id, .presentStageQueries = stage_mask };
        VkPresentTimingsInfoEXT tis = { .sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT, .pNext = has_id ? info->pNext : (const void *)&pid, .swapchainCount = 1, .pTimingInfos = &ti };
        VkPresentInfoKHR c = *info; c.pNext = &tis;
        logline("P %lld %ld\nQ %llu %d\n", (long long)t0, tid(), (unsigned long long)id, has_id);
        r = next_present(q, &c);
        logline("p %lld %ld\n", (long long)now_ns(), tid());
        harvest_timing();
    } else { logline("P %lld %ld\n", (long long)t0, tid()); r = next_present(q, info); logline("p %lld %ld\n", (long long)now_ns(), tid()); }
    return r; }

static VKAPI_ATTR void VKAPI_CALL vkpace_AntiLagUpdateAMD(VkDevice dev, const VkAntiLagDataAMD *d){
    int64_t t_in = now_ns();
    const VkAntiLagPresentationInfoAMD *pi = d ? d->pPresentationInfo : NULL;
    int stage = pi ? (int)pi->stage : -1; uint64_t frame = pi ? pi->frameIndex : 0;
    logline("A %lld %ld %d %u %d %llu\n", (long long)t_in, tid(), d ? (int)d->mode : -1, d ? d->maxFPS : 0, stage, (unsigned long long)frame);
    int pacing = T && d && d->mode != VK_ANTI_LAG_MODE_OFF_AMD && pi;
    int fwd_mode = d ? (int)d->mode : -1;
    if (next_antilag) {
        if (T && d) {
            VkAntiLagDataAMD c = *d; c.maxFPS = 0;
            if (driver_on && stage == VK_ANTI_LAG_STAGE_INPUT_AMD) {
                pthread_mutex_lock(&pmu);
                int discontinuity = (last_input_t && t_in - last_input_t > 3 * T) || (last_mode >= 0 && (int)d->mode != last_mode);
                if (discontinuity) driver_hold_until = t_in + driver_cooldown;
                last_input_t = t_in; last_mode = (int)d->mode; last_rebases = rebases;
                pthread_mutex_unlock(&pmu); }
            if (!driver_on || t_in < driver_hold_until) c.mode = VK_ANTI_LAG_MODE_OFF_AMD;
            fwd_mode = (int)c.mode; next_antilag(dev, &c); }
        else next_antilag(dev, d); }
    if (pacing) pace(stage, frame, t_in, now_ns());
    else if (T) { pthread_mutex_lock(&pmu); grid_valid = 0; last_exit = 0; prev_exit = 0; pthread_mutex_unlock(&pmu); }
    logline("a %lld %ld %d %llu %d\n", (long long)now_ns(), tid(), stage, (unsigned long long)frame, fwd_mode); }

static VKAPI_ATTR VkResult VKAPI_CALL vkpace_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *al, VkInstance *inst){
    VkLayerInstanceCreateInfo *li = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO)) li = (VkLayerInstanceCreateInfo *)li->pNext;
    if (!li) return VK_ERROR_INITIALIZATION_FAILED;
    next_gipa = li->u.pLayerInfo->pfnNextGetInstanceProcAddr; li->u.pLayerInfo = li->u.pLayerInfo->pNext;
    PFN_vkCreateInstance ci_fn = (PFN_vkCreateInstance)next_gipa(NULL, "vkCreateInstance");
    VkResult r = ci_fn(ci, al, inst); if (r == VK_SUCCESS) { g_instance = *inst; open_log(); } return r; }

static VKAPI_ATTR VkResult VKAPI_CALL vkpace_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *al, VkDevice *dev){
    VkLayerDeviceCreateInfo *li = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO)) li = (VkLayerDeviceCreateInfo *)li->pNext;
    if (!li) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = li->u.pLayerInfo->pfnNextGetInstanceProcAddr; next_gdpa = li->u.pLayerInfo->pfnNextGetDeviceProcAddr; li->u.pLayerInfo = li->u.pLayerInfo->pNext;
    next_create_device = (PFN_vkCreateDevice)gipa(NULL, "vkCreateDevice");
    int has_ext = 0; for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) if (!strcmp(ci->ppEnabledExtensionNames[i], "VK_AMD_anti_lag")) has_ext = 1;
    int feat = -1; for (const VkBaseInStructure *p = (const VkBaseInStructure *)ci->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD) feat = ((const VkPhysicalDeviceAntiLagFeaturesAMD *)p)->antiLag ? 1 : 0;
    const char *e = getenv("PRESENT_TIMING"); if (e) want_timing = atoi(e);
    /* the PyroFling latency layer (Mora) drains the same timing queue; leave it to it */
    else if ((e = getenv("LATENCY_MEASUREMENT")) && atoi(e)) want_timing = 0;
    else want_timing = log_on || jit_on;   /* flip feedback costs two driver calls per present; only the log and the scheduler use it */
    VkResult r = VK_ERROR_INITIALIZATION_FAILED; int added = 0;
    if (want_timing) {
        /* add the timing extensions and features; on any failure fall back to the game's own create info */
        const char *names[256]; uint32_t n = ci->enabledExtensionCount; if (n > 250) n = 250; memcpy(names, ci->ppEnabledExtensionNames, n * sizeof names[0]);
        int have_pt = 0, have_id2 = 0; for (uint32_t i = 0; i < n; i++) { if (!strcmp(names[i], "VK_EXT_present_timing")) have_pt = 1; if (!strcmp(names[i], "VK_KHR_present_id2")) have_id2 = 1; }
        if (!have_pt) names[n++] = "VK_EXT_present_timing"; if (!have_id2) names[n++] = "VK_KHR_present_id2";
        VkPhysicalDevicePresentId2FeaturesKHR fid = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, .pNext = (void *)ci->pNext, .presentId2 = VK_TRUE };
        VkPhysicalDevicePresentTimingFeaturesEXT fpt = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT, .pNext = &fid, .presentTiming = VK_TRUE };
        VkDeviceCreateInfo c = *ci; c.pNext = &fpt; c.enabledExtensionCount = n; c.ppEnabledExtensionNames = names;
        r = next_create_device(pd, &c, al, dev); added = r == VK_SUCCESS; }
    if (r != VK_SUCCESS) r = next_create_device(pd, ci, al, dev);
    if (r != VK_SUCCESS) return r;
    g_device = *dev;
    next_antilag = (PFN_vkAntiLagUpdateAMD)next_gdpa(*dev, "vkAntiLagUpdateAMD");
    next_present = (PFN_vkQueuePresentKHR)next_gdpa(*dev, "vkQueuePresentKHR");
    next_create_swapchain = (PFN_vkCreateSwapchainKHR)next_gdpa(*dev, "vkCreateSwapchainKHR");
    next_destroy_swapchain = (PFN_vkDestroySwapchainKHR)next_gdpa(*dev, "vkDestroySwapchainKHR");
    if (added) { next_set_queue_size = (PFN_vkSetSwapchainPresentTimingQueueSizeEXT)next_gdpa(*dev, "vkSetSwapchainPresentTimingQueueSizeEXT");
        next_time_domains = (PFN_vkGetSwapchainTimeDomainPropertiesEXT)next_gdpa(*dev, "vkGetSwapchainTimeDomainPropertiesEXT");
        next_past_timing = (PFN_vkGetPastPresentationTimingEXT)next_gdpa(*dev, "vkGetPastPresentationTimingEXT"); }
    logline("D %lld ext=%d feature=%d next=%d nexts=%u timing_ext=%d timing_fns=%d\n", (long long)now_ns(), has_ext, feat, next_antilag != NULL, ci->enabledExtensionCount, added, next_set_queue_size && next_time_domains && next_past_timing);
    next_destroy_device = (PFN_vkDestroyDevice)next_gdpa(*dev, "vkDestroyDevice"); return r; }

static VKAPI_ATTR void VKAPI_CALL vkpace_DestroyDevice(VkDevice dev, const VkAllocationCallbacks *al){ log_shutdown(); next_destroy_device(dev, al); }
__attribute__((destructor)) static void fini(void){ log_shutdown(); }

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkpace_GetDeviceProcAddr(VkDevice dev, const char *name){
    if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)vkpace_QueuePresentKHR;
    if (!strcmp(name, "vkAntiLagUpdateAMD")) { logline("G %lld %ld\n", (long long)now_ns(), tid()); return (PFN_vkVoidFunction)vkpace_AntiLagUpdateAMD; }
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)vkpace_CreateSwapchainKHR;
    if (!strcmp(name, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)vkpace_DestroySwapchainKHR;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)vkpace_DestroyDevice;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkpace_GetDeviceProcAddr;
    return next_gdpa ? next_gdpa(dev, name) : NULL; }
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkpace_GetInstanceProcAddr(VkInstance inst, const char *name){
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)vkpace_CreateInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)vkpace_CreateDevice;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkpace_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkpace_GetDeviceProcAddr;
    if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)vkpace_QueuePresentKHR;
    if (!strcmp(name, "vkAntiLagUpdateAMD")) return (PFN_vkVoidFunction)vkpace_AntiLagUpdateAMD;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)vkpace_CreateSwapchainKHR;
    if (!strcmp(name, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)vkpace_DestroySwapchainKHR;
    return next_gipa ? next_gipa(inst, name) : NULL; }
VKAPI_ATTR VkResult VKAPI_CALL vkpace_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v){
    if (v->loaderLayerInterfaceVersion >= 2) { v->loaderLayerInterfaceVersion = 2; v->pfnGetInstanceProcAddr = vkpace_GetInstanceProcAddr; v->pfnGetDeviceProcAddr = vkpace_GetDeviceProcAddr; v->pfnGetPhysicalDeviceProcAddr = NULL; }
    return VK_SUCCESS; }
