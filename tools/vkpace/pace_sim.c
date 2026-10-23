// Game-loop simulator for the pacer: input-stage anti-lag call, busy work, present-stage call.
// Work is light (SIM_LIGHT_MS) except in frames that process a 64 Hz tick (SIM_HEAVY_MS),
// which reproduces Deadlock's bimodal main-thread cost. No swapchain, nothing drawn.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int64_t now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }
static void busy(double ms){ int64_t end = now() + (int64_t)(ms * 1e6); while (now() < end) __builtin_ia32_pause(); }
static double envd(const char *n, double d){ const char *e = getenv(n); return e ? atof(e) : d; }
int main(void){
    int frames = (int)envd("SIM_FRAMES", 1500); double light = envd("SIM_LIGHT_MS", 5.0), heavy = envd("SIM_HEAVY_MS", 7.9), tick = 1000.0 / envd("SIM_TICK_HZ", 64);
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst)) return 1;
    uint32_t n = 8; VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(inst, &n, pds); VkPhysicalDevice pd = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); if (p.vendorID == 0x1002) pd = pds[i]; }
    if (!pd) return 1;
    float prio = 1.0f; VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceAntiLagFeaturesAMD alf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD, .antiLag = VK_TRUE };
    const char *exts[] = { "VK_AMD_anti_lag" };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &alf, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = exts };
    VkDevice dev; if (vkCreateDevice(pd, &dci, NULL, &dev)) return 2;
    PFN_vkAntiLagUpdateAMD upd = (PFN_vkAntiLagUpdateAMD)vkGetDeviceProcAddr(dev, "vkAntiLagUpdateAMD"); if (!upd) return 3;
    double t0 = now() / 1e6, next_tick = t0 + tick; int heavy_n = 0;
    for (uint64_t f = 1; f <= (uint64_t)frames; f++) {
        VkAntiLagPresentationInfoAMD pi = { .sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD, .stage = VK_ANTI_LAG_STAGE_INPUT_AMD, .frameIndex = f };
        VkAntiLagDataAMD d = { .sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD, .mode = VK_ANTI_LAG_MODE_ON_AMD, .maxFPS = (uint32_t)envd("SIM_MAXFPS", 0), .pPresentationInfo = &pi };
        upd(dev, &d);
        if (f == (uint64_t)envd("SIM_PAUSE_AT", 0)) busy(envd("SIM_PAUSE_MS", 100));   /* simulated alt-tab / menu stall */
        double t = now() / 1e6; int is_heavy = t >= next_tick; while (t >= next_tick) next_tick += tick;
        busy(is_heavy ? heavy : light); heavy_n += is_heavy;
        pi.stage = VK_ANTI_LAG_STAGE_PRESENT_AMD; upd(dev, &d); }
    fprintf(stderr, "sim: %d frames, %d heavy, %.1f s\n", frames, heavy_n, (now() / 1e6 - t0) / 1e3);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL); return 0; }
