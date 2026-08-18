// SPDX-License-Identifier: MIT
//
// openxr-stub — the server half of an ovrtx XR app, up to the point where
// this machine runs out of hardware.
//
// What it does today:
//   * loads the OpenXR loader and lists visible API layers + extensions
//   * creates an XrInstance requesting XR_KHR_vulkan_enable2 (the path an
//     ovrtx app would take, since ovrtx hands out Vulkan/CUDA images)
//   * asks for the HMD system and prints the runtime's recommended
//     per-eye render target size and swapchain formats
//
// On a box with no OpenXR runtime installed — no /usr/share/openxr/1/,
// no Monado, no SteamVR, no CloudXR Runtime — xrCreateInstance returns
// XR_ERROR_RUNTIME_UNAVAILABLE (-2). That is the expected, correct
// outcome here, and the program says so rather than pretending.
//
// The commented block at the bottom of run() is the actual integration
// contract with ovrtx. It is deliberately not written as dead code: the
// per-eye projection math depends on the runtime's FoV, which we cannot
// obtain without a runtime, and guessing it would produce code that
// compiles, looks finished, and is wrong.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <openxr/openxr.h>

static const char *result_name(XrInstance instance, XrResult r)
{
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, r, buf)))
        return buf;
    switch (r) {
    case XR_ERROR_RUNTIME_UNAVAILABLE:   return "XR_ERROR_RUNTIME_UNAVAILABLE";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
    case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
    default:                             break;
    }
    snprintf(buf, sizeof buf, "XrResult %d", (int)r);
    return buf;
}

static void list_extensions(void)
{
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(NULL, 0, &count, NULL))) {
        printf("  (could not enumerate extensions)\n");
        return;
    }
    if (count == 0) {
        printf("  none — no runtime is providing extensions\n");
        return;
    }

    XrExtensionProperties *props = calloc(count, sizeof *props);
    for (uint32_t i = 0; i < count; ++i)
        props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, count, &count, props)))
        for (uint32_t i = 0; i < count; ++i)
            printf("  %s (v%u)\n", props[i].extensionName, props[i].extensionVersion);
    free(props);
}

static int run(void)
{
    printf("OpenXR instance extensions visible to the loader:\n");
    list_extensions();

    // XR_KHR_vulkan_enable2 is the relevant one for ovrtx: the runtime
    // picks the VkInstance/VkDevice, and ovrtx is then created against
    // that same device so its output images can be blitted into the
    // runtime's swapchain without a round trip through host memory.
    const char *extensions[] = { "XR_KHR_vulkan_enable2" };

    XrInstanceCreateInfo create_info = { .type = XR_TYPE_INSTANCE_CREATE_INFO };
    create_info.enabledExtensionCount = 1;
    create_info.enabledExtensionNames = extensions;
    strncpy(create_info.applicationInfo.applicationName, "ovrtx-openxr-stub",
            XR_MAX_APPLICATION_NAME_SIZE - 1);
    create_info.applicationInfo.applicationVersion = 1;
    strncpy(create_info.applicationInfo.engineName, "ovrtx",
            XR_MAX_ENGINE_NAME_SIZE - 1);
    create_info.applicationInfo.engineVersion = 1;
    create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    XrInstance instance = XR_NULL_HANDLE;
    XrResult res = xrCreateInstance(&create_info, &instance);
    if (XR_FAILED(res)) {
        printf("\nxrCreateInstance failed: %s\n", result_name(XR_NULL_HANDLE, res));
        if (res == XR_ERROR_RUNTIME_UNAVAILABLE) {
            printf(
                "\nNo OpenXR runtime is installed on this machine. Expected —\n"
                "this stub builds to prove the toolchain, it cannot run without\n"
                "one of:\n"
                "  * NVIDIA CloudXR Runtime  (the streaming path; see ../README.md)\n"
                "  * Monado                  (open source, has a simulated HMD driver)\n"
                "  * SteamVR                 (needs real hardware)\n"
                "A runtime advertises itself via an active_runtime.json under\n"
                "/usr/share/openxr/1/, /etc/xdg/openxr/1/ or ~/.config/openxr/1/.\n");
        } else if (res == XR_ERROR_EXTENSION_NOT_PRESENT) {
            printf("\nA runtime is present but does not support "
                   "XR_KHR_vulkan_enable2.\n");
        } else if (res == XR_ERROR_RUNTIME_FAILURE) {
            printf(
                "\nA runtime loaded but could not create an instance. For the\n"
                "CloudXR runtime this normally means its service process is not\n"
                "running: CloudXR is Monado-based and split into a client .so\n"
                "plus a daemon that owns /run/user/$UID/ipc_cloudxr. Kit ships\n"
                "only the client half — the daemon comes from the CloudXR\n"
                "Runtime package on NGC. See ../README.md.\n");
        }
        return 0;   // not a build/test failure — a missing-hardware report
    }

    XrInstanceProperties inst_props = { .type = XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &inst_props)))
        printf("\nRuntime: %s (v%u.%u.%u)\n", inst_props.runtimeName,
               XR_VERSION_MAJOR(inst_props.runtimeVersion),
               XR_VERSION_MINOR(inst_props.runtimeVersion),
               XR_VERSION_PATCH(inst_props.runtimeVersion));

    XrSystemGetInfo system_info = { .type = XR_TYPE_SYSTEM_GET_INFO };
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId system_id = XR_NULL_SYSTEM_ID;
    res = xrGetSystem(instance, &system_info, &system_id);
    if (XR_FAILED(res)) {
        printf("xrGetSystem failed: %s\n", result_name(instance, res));
        printf("A runtime is loaded but no headset is connected.\n");
        xrDestroyInstance(instance);
        return 0;
    }

    // Per-eye render target size. This is the number that decides whether
    // the whole idea is viable: two ovrtx RenderProducts at this size,
    // path-traced, at the display refresh rate.
    uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(instance, system_id,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      0, &view_count, NULL);
    if (view_count) {
        XrViewConfigurationView *views = calloc(view_count, sizeof *views);
        for (uint32_t i = 0; i < view_count; ++i)
            views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(
                instance, system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                view_count, &view_count, views))) {
            printf("\nStereo view configuration: %u views\n", view_count);
            for (uint32_t i = 0; i < view_count; ++i)
                printf("  view %u: recommended %ux%u, max %ux%u, %u sample(s)\n", i,
                       views[i].recommendedImageRectWidth,
                       views[i].recommendedImageRectHeight,
                       views[i].maxImageRectWidth,
                       views[i].maxImageRectHeight,
                       views[i].recommendedSwapchainSampleCount);
        }
        free(views);
    }

    // --- Where ovrtx would plug in -------------------------------------
    //
    // Everything above needs only the loader. Everything below needs a
    // live session, which needs a runtime, which this machine does not
    // have — so it is described rather than stubbed:
    //
    //   1. xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR — the
    //      runtime chooses the VkPhysicalDevice. ovrtx must be created
    //      against that same device or every frame costs a copy across
    //      GPUs.
    //   2. xrCreateSwapchain per eye, at recommendedImageRect*.
    //   3. Author two Cameras + two RenderProducts in the USD stage, one
    //      per eye, at that resolution.
    //   4. Per frame:
    //        xrWaitFrame -> xrBeginFrame
    //        xrLocateViews(predictedDisplayTime) -> per-eye pose + FoV
    //        write each eye pose into its camera (omni:xform) and the
    //          asymmetric FoV into the camera's aperture/offset
    //        renderer.step({left_product, right_product})
    //        map each LdrColor and blit into the acquired swapchain image
    //        xrEndFrame with an XrCompositionLayerProjection
    //
    // The hard part is not any of those calls. It is that ovrtx's RT2
    // mode accumulates samples over frames, and head motion invalidates
    // that accumulation every frame. Budget the work against convergence,
    // not against the API surface.

    xrDestroyInstance(instance);
    return 0;
}

int main(void)
{
    printf("ovrtx OpenXR stub — probing the local XR stack\n");
    printf("==============================================\n\n");
    return run();
}
