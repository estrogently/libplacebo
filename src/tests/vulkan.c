#include "gpu_tests.h"
#include "vulkan/command.h"
#include "vulkan/gpu.h"
#include <vulkan/vulkan.h>

static void vulkan_interop_tests(pl_vulkan pl_vk,
                                 enum pl_handle_type handle_type)
{
    pl_gpu gpu = pl_vk->gpu;
    printf("testing vulkan interop for handle type 0x%x\n", handle_type);

    if (gpu->export_caps.buf & handle_type) {
        pl_buf buf = pl_buf_create(gpu, &(struct pl_buf_params) {
            .size = 1024,
            .export_handle = handle_type,
        });

        REQUIRE(buf);
        REQUIRE_HANDLE(buf->shared_mem, handle_type);
        REQUIRE(buf->shared_mem.size >= buf->params.size);
        REQUIRE(pl_buf_export(gpu, buf));
        pl_buf_destroy(gpu, &buf);
    }

    pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 0, 0, PL_FMT_CAP_BLITTABLE);
    if (!fmt)
        return;

    if (gpu->export_caps.sync & handle_type) {
        pl_sync sync = pl_sync_create(gpu, handle_type);
        pl_tex tex = pl_tex_create(gpu, &(struct pl_tex_params) {
            .w = 32,
            .h = 32,
            .format = fmt,
            .blit_dst = true,
        });

        REQUIRE(sync);
        REQUIRE(tex);

        // Note: For testing purposes, we have to fool pl_tex_export into
        // thinking this texture is actually exportable. Just hack it in
        // horribly.
        ((struct pl_tex_params *) &tex->params)->export_handle = PL_HANDLE_DMA_BUF;

        REQUIRE(pl_tex_export(gpu, tex, sync));

        // Re-use our internal helpers to signal this VkSemaphore
        struct vk_ctx *vk = PL_PRIV(pl_vk);
        struct vk_cmd *cmd = vk_cmd_begin(vk, vk->pool_graphics);
        VkSemaphore signal;
        REQUIRE(cmd);
        pl_vk_sync_unwrap(sync, NULL, &signal);
        vk_cmd_sig(cmd, signal);
        vk_cmd_queue(vk, &cmd);
        REQUIRE(vk_flush_commands(vk));

        // Do something with the image again to "import" it
        pl_tex_clear(gpu, tex, (float[4]){0});
        pl_gpu_finish(gpu);
        REQUIRE(!pl_tex_poll(gpu, tex, 0));

        pl_sync_destroy(gpu, &sync);
        pl_tex_destroy(gpu, &tex);
    }
}

static void vulkan_swapchain_tests(pl_vulkan vk, VkSurfaceKHR surf)
{
    if (!surf)
        return;

    printf("testing vulkan swapchain\n");
    pl_gpu gpu = vk->gpu;
    pl_swapchain sw;
    sw = pl_vulkan_create_swapchain(vk, &(struct pl_vulkan_swapchain_params) {
        .surface = surf,
    });
    REQUIRE(sw);

    // Attempt actually initializing the swapchain
    int w = 640, h = 480;
    REQUIRE(pl_swapchain_resize(sw, &w, &h));

    for (int i = 0; i < 10; i++) {
        struct pl_swapchain_frame frame;
        REQUIRE(pl_swapchain_start_frame(sw, &frame));
        if (frame.fbo->params.blit_dst)
            pl_tex_clear(gpu, frame.fbo, (float[4]){0});

        // TODO: test this with an actual pl_renderer instance
        struct pl_frame target;
        pl_frame_from_swapchain(&target, &frame);

        REQUIRE(pl_swapchain_submit_frame(sw));
        pl_swapchain_swap_buffers(sw);

        // Try resizing the swapchain in the middle of rendering
        if (i == 5) {
            w = 320;
            h = 240;
            REQUIRE(pl_swapchain_resize(sw, &w, &h));
        }
    }

    pl_swapchain_destroy(&sw);
}

int main()
{
    pl_log log = pl_test_logger();
    pl_vk_inst inst = pl_vk_inst_create(log, &(struct pl_vk_inst_params) {
        .debug = true,
        .debug_extra = true,
        .get_proc_addr = vkGetInstanceProcAddr,
        .opt_extensions = (const char *[]){
            VK_KHR_SURFACE_EXTENSION_NAME,
            "VK_EXT_headless_surface", // in case it isn't defined
        },
        .num_opt_extensions = 2,
    });

    if (!inst)
        return SKIP;

    PL_VK_LOAD_FUN(inst->instance, EnumeratePhysicalDevices, inst->get_proc_addr);
    PL_VK_LOAD_FUN(inst->instance, GetPhysicalDeviceProperties, inst->get_proc_addr);

    uint32_t num = 0;
    EnumeratePhysicalDevices(inst->instance, &num, NULL);
    if (!num)
        return SKIP;

    VkPhysicalDevice *devices = calloc(num, sizeof(*devices));
    if (!devices)
        return 1;
    EnumeratePhysicalDevices(inst->instance, &num, devices);

    VkSurfaceKHR surf = NULL;

#ifdef VK_EXT_headless_surface
    PL_VK_LOAD_FUN(inst->instance, CreateHeadlessSurfaceEXT, inst->get_proc_addr);
    if (CreateHeadlessSurfaceEXT) {
        VkHeadlessSurfaceCreateInfoEXT info = {
            .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT,
        };

        VkResult res = CreateHeadlessSurfaceEXT(inst->instance, &info, NULL, &surf);
        REQUIRE(res == VK_SUCCESS);
    }
#endif // VK_EXT_headless_surface

    // Make sure choosing any device works
    VkPhysicalDevice dev;
    dev = pl_vulkan_choose_device(log, &(struct pl_vulkan_device_params) {
        .instance = inst->instance,
        .get_proc_addr = inst->get_proc_addr,
        .allow_software = true,
        .surface = surf,
    });
    REQUIRE(dev);

    // Test all attached devices
    for (int i = 0; i < num; i++) {
        VkPhysicalDeviceProperties props = {0};
        GetPhysicalDeviceProperties(devices[i], &props);
        printf("Testing device %d: %s\n", i, props.deviceName);

        // Make sure we can choose this device by name
        dev = pl_vulkan_choose_device(log, &(struct pl_vulkan_device_params) {
            .instance = inst->instance,
            .get_proc_addr = inst->get_proc_addr,
            .device_name = props.deviceName,
        });
        REQUIRE(dev == devices[i]);

        struct pl_vulkan_params params = pl_vulkan_default_params;
        params.instance = inst->instance;
        params.get_proc_addr = inst->get_proc_addr;
        params.device = devices[i];
        params.queue_count = 8; // test inter-queue stuff
        params.surface = surf;

        pl_vulkan vk = pl_vulkan_create(log, &params);
        if (!vk)
            continue;

        gpu_shader_tests(vk->gpu);
        vulkan_swapchain_tests(vk, surf);

        // Test importing this context via the vulkan interop API
        struct pl_vulkan_import_params iparams = {
            .instance = vk->instance,
            .get_proc_addr = inst->get_proc_addr,
            .phys_device = vk->phys_device,
            .device = vk->device,

            .extensions = vk->extensions,
            .num_extensions = vk->num_extensions,
            .features = vk->features,
            .queue_graphics = vk->queue_graphics,
            .queue_compute = vk->queue_compute,
            .queue_transfer = vk->queue_transfer,
        };
        pl_vulkan vk2 = pl_vulkan_import(log, &iparams);
        REQUIRE(vk2);
        pl_vulkan_destroy(&vk2);

        // Run these tests last because they disable some validation layers
#ifdef PL_HAVE_UNIX
        vulkan_interop_tests(vk, PL_HANDLE_FD);
        vulkan_interop_tests(vk, PL_HANDLE_DMA_BUF);
#endif
#ifdef PL_HAVE_WIN32
        vulkan_interop_tests(vk, PL_HANDLE_WIN32);
        vulkan_interop_tests(vk, PL_HANDLE_WIN32_KMT);
#endif
        gpu_interop_tests(vk->gpu);
        pl_vulkan_destroy(&vk);

        // Re-run the same export/import tests with async queues disabled
        params.async_compute = false;
        params.async_transfer = false;
        vk = pl_vulkan_create(log, &params);
        REQUIRE(vk); // it succeeded the first time

#ifdef PL_HAVE_UNIX
        vulkan_interop_tests(vk, PL_HANDLE_FD);
        vulkan_interop_tests(vk, PL_HANDLE_DMA_BUF);
#endif
#ifdef PL_HAVE_WIN32
        vulkan_interop_tests(vk, PL_HANDLE_WIN32);
        vulkan_interop_tests(vk, PL_HANDLE_WIN32_KMT);
#endif
        gpu_interop_tests(vk->gpu);
        pl_vulkan_destroy(&vk);

        // Reduce log spam after first tested device
        pl_log_level_update(log, PL_LOG_INFO);
    }

    vkDestroySurfaceKHR(inst->instance, surf, NULL);
    pl_vk_inst_destroy(&inst);
    pl_log_destroy(&log);
    free(devices);
}
