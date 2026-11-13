// Driver calls are spies; lifecycle/teardown/acquire come verbatim from device.c.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VK_NULL_HANDLE 0
#define VK_SC_MAX 8
#define FRAMES 3
enum { VK_SUCCESS, VK_SUBOPTIMAL_KHR, VK_TIMEOUT, VK_NOT_READY, VK_ERROR_OUT_OF_DATE_KHR, VK_ERROR_SURFACE_LOST_KHR };
typedef uint64_t VkSemaphore;
typedef int VkResult;
static struct { uint64_t inst, dev, surface, swapchain, sc_views[VK_SC_MAX]; unsigned sc_count; struct { unsigned width, height; } sc_extent; } g_vk;
static uint64_t sc_fb[VK_SC_MAX], hdr_fb[VK_SC_MAX];
static VkSemaphore acquire_sem[FRAMES], done_sem[VK_SC_MAX];
static uint64_t slot_seq[FRAMES];
static int slot;
static void *surface_window, *window_now;
static bool surface_lost, bg_cache_saved;
static volatile int g_android_window_state;
static struct { unsigned image; VkSemaphore acquired; bool ok; } cur_frame;
static struct { int msaa; } g_config;
static int configured_msaa, swapchain_vsync, swapchain_hdr;
static int creates, direct_sdl, direct_destroy, swap_creates, idle_count, invalid_lifetime, surface_fail, swap_fail, background_during_create;
static int live_fb, live_views, acquire_calls, recreate_fail, recreate_bg;
static VkResult acquire_results[2];
static uint64_t timeouts[2], next_id;
#define LOG(...) ((void)0)
static void *ui_window(void) { return window_now; }
static void *android_window(void) { return window_now; }
static const char *SDL_GetError(void) { return "injected"; }
static void SDL_GetWindowSizeInPixels(void *w, int *x, int *y) { (void)w; *x=2772; *y=1280; }
static bool ui_window_away(void) { return g_android_window_state==1; }
static uint64_t vk_pending_seq(void) { return 1; }
static void vk_wait_seq(uint64_t s) { (void)s; }
static void vk_wait_site(int s) { (void)s; }
static void vkDeviceWaitIdle(uint64_t d) { (void)d; idle_count++; }
static void vkDestroyFramebuffer(uint64_t d, uint64_t f, void *a) {
    (void)d; (void)a; if (f) { if (!g_vk.swapchain) invalid_lifetime++; live_fb--; }
}
static void vkDestroyImageView(uint64_t d, uint64_t v, void *a) {
    (void)d; (void)a; if (v) { if (!g_vk.swapchain || live_fb) invalid_lifetime++; live_views--; }
}
static void vkDestroySemaphore(uint64_t d, uint64_t s, void *a) { (void)d; (void)s; (void)a; }
static void vkDestroySwapchainKHR(uint64_t d, uint64_t s, void *a) { (void)d; (void)s; (void)a; if (live_fb || live_views) invalid_lifetime++; }
static void vkDestroySurfaceKHR(uint64_t i, uint64_t s, void *a) { (void)i; (void)s; (void)a; if (g_vk.swapchain) invalid_lifetime++; }
static void SDL_Vulkan_DestroySurface(uint64_t i, uint64_t s, void *a) { direct_destroy++; vkDestroySurfaceKHR(i,s,a); }
static bool create_surface(void) {
    creates++; if (surface_fail) { surface_fail--; return false; }
    assert(window_now); g_vk.surface=++next_id; return true;
}
#define make_surface() create_surface()
static bool SDL_Vulkan_CreateSurface(void *w, uint64_t i, void *a, uint64_t *s) {
    (void)w; (void)i; (void)a; (void)s; direct_sdl++; return create_surface();
}
static void create_swapchain(void) {
    swap_creates++; assert(g_vk.surface);
    if (swap_fail) { swap_fail--; surface_lost=true; return; }
    for (unsigned i=0; i<g_vk.sc_count; i++) {
        vkDestroyFramebuffer(g_vk.dev,sc_fb[i],0); vkDestroyFramebuffer(g_vk.dev,hdr_fb[i],0);
        sc_fb[i]=hdr_fb[i]=0; vkDestroyImageView(g_vk.dev,g_vk.sc_views[i],0);
    }
    if (g_vk.swapchain) vkDestroySwapchainKHR(g_vk.dev,g_vk.swapchain,0);
    g_vk.swapchain=++next_id; g_vk.sc_count=1; g_vk.sc_extent.width=2772; g_vk.sc_extent.height=1280;
    sc_fb[0]=++next_id; g_vk.sc_views[0]=++next_id; live_fb=live_views=1;
    if (background_during_create) g_android_window_state=1;
}
static void recreate_swapchain(void) {
    if (recreate_bg) g_android_window_state=1;
    if (recreate_fail) { surface_lost=true; g_vk.swapchain=0; return; }
    create_swapchain();
}
static int config_live_msaa(void) { return 0; }
static void vk_set_msaa(unsigned v) { (void)v; }
static int config_live_vsync(void) { return 0; }
static int hdr_wanted(void) { return 0; }
static uint64_t binary_semaphore(void) { return ++next_id; }
static void background_cache_save(void) { bg_cache_saved=true; }
static VkResult vkAcquireNextImageKHR(uint64_t d, uint64_t sc, uint64_t timeout, uint64_t sem, uint64_t f, unsigned *im) {
    (void)d; (void)sem; (void)f; assert(sc); assert(acquire_calls<2);
    timeouts[acquire_calls]=timeout; *im=0; return acquire_results[acquire_calls++];
}
#include "surface_under_test.h"
static void reset(void) {
    memset(&g_vk,0,sizeof g_vk); memset(sc_fb,0,sizeof sc_fb); memset(hdr_fb,0,sizeof hdr_fb);
    memset(acquire_sem,0,sizeof acquire_sem); memset(done_sem,0,sizeof done_sem); memset(slot_seq,0,sizeof slot_seq);
    memset(acquire_results,0,sizeof acquire_results); memset(timeouts,0,sizeof timeouts);
    creates=direct_sdl=direct_destroy=swap_creates=idle_count=invalid_lifetime=surface_fail=swap_fail=background_during_create=0;
    live_fb=live_views=acquire_calls=recreate_fail=recreate_bg=0; slot=0; surface_lost=bg_cache_saved=false;
    g_android_window_state=0; window_now=(void *)1; surface_window=window_now; g_vk.surface=++next_id;
    create_swapchain(); swap_creates=0;
}
int main(void) {
    for (int i=0; i<1000; i++) {
        reset(); window_now=(void *)2; g_android_window_state=2;
        android_window_check(); assert(surface_ready());
#if REFERENCE_BUILD
        assert(creates==2 && swap_creates==2 && invalid_lifetime>0 && direct_sdl==1 && direct_destroy==1);
#else
        assert(creates==1 && swap_creates==1 && invalid_lifetime==0 && direct_sdl==0 && direct_destroy==0);
#endif
        assert(g_android_window_state==0 && surface_window==window_now);
    }
#if REFERENCE_BUILD
    reset(); acquire_results[0]=VK_TIMEOUT; assert(!frame_begin()); assert(timeouts[0]==UINT64_MAX);
    puts("PASS baseline: 1000 resumes reproduce duplicate rebuild/invalid lifetime and unbounded acquire.");
#else
    reset(); g_android_window_state=2; android_window_check(); assert(surface_ready());
    assert(creates==1 && invalid_lifetime==0); // Same pointer may be reused by Android.
    reset(); window_now=0; g_android_window_state=2; android_window_check();
    assert(g_android_window_state==2 && !g_vk.surface && !g_vk.swapchain && creates==0);
    window_now=(void *)3; android_window_check(); assert(g_android_window_state==0 && surface_ready());
    reset(); g_android_window_state=2; surface_fail=1; android_window_check();
    assert(g_android_window_state==2 && !g_vk.surface && !g_vk.swapchain);
    android_window_check(); assert(g_android_window_state==0 && creates==2 && !invalid_lifetime);
    reset(); g_android_window_state=2; swap_fail=1; android_window_check();
    assert(g_android_window_state==2 && !g_vk.swapchain);
    android_window_check(); assert(g_android_window_state==0 && !invalid_lifetime);
    reset(); g_android_window_state=2; background_during_create=1; assert(!frame_begin());
    assert(g_android_window_state==1 && acquire_calls==0);
    background_during_create=0; g_android_window_state=2; assert(frame_begin());
    reset(); g_android_window_state=1; assert(!frame_begin()); assert(bg_cache_saved && !acquire_calls);
    reset(); surface_lost=true; assert(frame_begin()); assert(!invalid_lifetime && direct_sdl==0);
    for (int status=VK_TIMEOUT; status<=VK_NOT_READY; status++) {
        reset(); acquire_results[0]=status; assert(!frame_begin());
        assert(!cur_frame.ok && timeouts[0]==100000000ull);
        acquire_calls=0; acquire_results[0]=VK_SUCCESS; assert(frame_begin());
    }
    reset(); acquire_results[0]=VK_ERROR_OUT_OF_DATE_KHR; acquire_results[1]=VK_SUCCESS;
    assert(frame_begin()); assert(acquire_calls==2 && timeouts[1]==100000000ull);
    reset(); acquire_results[0]=VK_ERROR_OUT_OF_DATE_KHR; recreate_fail=1; assert(!frame_begin()); assert(acquire_calls==1);
    reset(); acquire_results[0]=VK_ERROR_OUT_OF_DATE_KHR; recreate_bg=1; assert(!frame_begin()); assert(acquire_calls==1);
    reset(); acquire_results[0]=VK_ERROR_SURFACE_LOST_KHR; assert(!frame_begin()); assert(surface_lost);
    acquire_calls=0; acquire_results[0]=VK_SUCCESS; assert(frame_begin()); assert(!surface_lost);
    puts("PASS fixed: 1000 resumes; reused/missing windows, creation failures, repeated background, surface loss, timeout/retry.");
#endif
    return 0;
}
