#include "ui_overlay.h"
#include "settings.h"
#include "hardware_stats.h"
#include "present.h"
#include "input_bindings.h"
#include "../gpu/tex_replace.h"
#include "../gpu/ge.h"
#include "pspreco.h"
#include "../hle/game_state.h"
#include <math.h>

extern "C" u8 *g_ram;

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <string.h>
#include <float.h>
#include <vector>

#define MAX_IMAGES 8

static bool            s_inited;
static bool            s_backend_up;
static bool            s_visible;
static bool            s_readout_changed;
static bool            s_advanced;
static bool            s_saved;
static uint64_t        s_saved_until;
static SDL_Window     *s_win;
static VkInstance      s_inst;
static VkPhysicalDevice s_pdev;
static VkDevice        s_dev;
static VkQueue         s_queue;
static uint32_t        s_qfam;

static VkRenderPass    s_pass;
static VkFormat        s_fmt = VK_FORMAT_UNDEFINED;
static VkExtent2D      s_ext;
static uint32_t        s_n_img;
static VkImageView     s_views[MAX_IMAGES];
static VkFramebuffer   s_fbs[MAX_IMAGES];

static double   s_fps, s_frame_ms, s_game_fps;
static uint64_t s_last_ticks;
static int      s_frames;
static unsigned s_last_game_frames;

extern "C" unsigned long g_ge_prim_count;
extern "C" unsigned display_game_frames(void);

static void destroy_targets(void)
{
    for (uint32_t i = 0; i < s_n_img; i++) {
        if (s_fbs[i])   vkDestroyFramebuffer(s_dev, s_fbs[i], NULL);
        if (s_views[i]) vkDestroyImageView(s_dev, s_views[i], NULL);
        s_fbs[i] = VK_NULL_HANDLE;
        s_views[i] = VK_NULL_HANDLE;
    }
    s_n_img = 0;
}

static bool make_render_pass(VkFormat fmt)
{
    VkAttachmentDescription att;
    VkAttachmentReference   ref;
    VkSubpassDescription    sub;
    VkSubpassDependency     dep;
    VkRenderPassCreateInfo  rpi;

    if (s_pass) { vkDestroyRenderPass(s_dev, s_pass, NULL); s_pass = VK_NULL_HANDLE; }

    memset(&att, 0, sizeof att);
    att.format         = fmt;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout  = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    att.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    memset(&ref, 0, sizeof ref);
    ref.attachment = 0;
    ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    memset(&sub, 0, sizeof sub);
    sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments    = &ref;

    memset(&dep, 0, sizeof dep);
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                      | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    memset(&rpi, 0, sizeof rpi);
    rpi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1;
    rpi.pAttachments    = &att;
    rpi.subpassCount    = 1;
    rpi.pSubpasses      = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies   = &dep;
    return vkCreateRenderPass(s_dev, &rpi, NULL, &s_pass) == VK_SUCCESS;
}

int ui_init(const UiVkInfo *vk)
{
    if (!vk || !vk->window || s_inited) return s_inited ? 1 : 0;
    s_win   = (SDL_Window *)vk->window;
    s_inst  = (VkInstance)vk->instance;
    s_pdev  = (VkPhysicalDevice)vk->physical;
    s_dev   = (VkDevice)vk->device;
    s_queue = (VkQueue)vk->queue;
    s_qfam  = vk->queue_family;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    ImFontConfig font;
    font.SizePixels = 16.0f;
    io.Fonts->AddFontDefault(&font);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard
                    | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 4.0f;
    st.FrameRounding  = 3.0f;
    st.GrabRounding   = 3.0f;
    st.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    st.Colors[ImGuiCol_WindowBg].w = 0.94f;

    if (!ImGui_ImplSDL3_InitForVulkan(s_win)) {
        ImGui::DestroyContext();
        return 0;
    }
    s_inited = true;
    s_last_ticks = SDL_GetTicks();
    VkPhysicalDeviceIDProperties id = {};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDevicePCIBusInfoPropertiesEXT pci = {};
    pci.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
    uint32_t n_ext = 0;
    bool have_pci = false;
    if (vkEnumerateDeviceExtensionProperties(s_pdev, NULL, &n_ext, NULL) == VK_SUCCESS) {
        std::vector<VkExtensionProperties> ext(n_ext);
        if (vkEnumerateDeviceExtensionProperties(s_pdev, NULL, &n_ext, ext.data()) == VK_SUCCESS)
            for (const auto &e : ext) if (!strcmp(e.extensionName, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME)) have_pci = true;
    }
    if (have_pci) id.pNext = &pci;
    VkPhysicalDeviceProperties2 props = {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &id;
    vkGetPhysicalDeviceProperties2(s_pdev, &props);
    char nvidia_bus[32] = {};
    if (have_pci && props.properties.vendorID == 0x10de)
        snprintf(nvidia_bus, sizeof nvidia_bus, "%08x:%02x:%02x.%x", pci.pciDomain, pci.pciBus, pci.pciDevice, pci.pciFunction);
    hardware_stats_init(props.properties.deviceName, id.deviceLUID, id.deviceLUIDValid != 0, nvidia_bus);
    return 1;
}

void ui_swapchain(uint32_t format, uint32_t width, uint32_t height,
                  uint32_t count, void *const *images)
{
    if (!s_inited || !count) return;
    if (count > MAX_IMAGES) count = MAX_IMAGES;

    destroy_targets();
    s_ext.width = width;
    s_ext.height = height;

    if ((VkFormat)format != s_fmt) {
        s_fmt = (VkFormat)format;
        if (s_backend_up) { ImGui_ImplVulkan_Shutdown(); s_backend_up = false; }
        if (!make_render_pass(s_fmt)) return;
    }

    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo vci;
        VkFramebufferCreateInfo fci;
        memset(&vci, 0, sizeof vci);
        vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image    = (VkImage)images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = s_fmt;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(s_dev, &vci, NULL, &s_views[i]) != VK_SUCCESS) return;

        memset(&fci, 0, sizeof fci);
        fci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass      = s_pass;
        fci.attachmentCount = 1;
        fci.pAttachments    = &s_views[i];
        fci.width           = width;
        fci.height          = height;
        fci.layers          = 1;
        if (vkCreateFramebuffer(s_dev, &fci, NULL, &s_fbs[i]) != VK_SUCCESS) return;
        s_n_img = i + 1;
    }

    if (!s_backend_up) {
        ImGui_ImplVulkan_InitInfo ii = {};
        ii.ApiVersion         = VK_API_VERSION_1_0;
        ii.Instance           = s_inst;
        ii.PhysicalDevice     = s_pdev;
        ii.Device             = s_dev;
        ii.QueueFamily        = s_qfam;
        ii.Queue              = s_queue;
        ii.DescriptorPoolSize = 16;
        ii.MinImageCount      = count < 2 ? 2 : count;
        ii.ImageCount         = count < 2 ? 2 : count;
        ii.PipelineInfoMain.RenderPass  = s_pass;
        ii.PipelineInfoMain.Subpass     = 0;
        ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        s_backend_up = ImGui_ImplVulkan_Init(&ii);
        if (!s_backend_up)
            fprintf(stderr, "[ui] ImGui Vulkan backend failed; overlay off\n");
    } else {
        ImGui_ImplVulkan_SetMinImageCount(count < 2 ? 2 : count);
    }
}

void ui_shutdown(void)
{
    if (!s_inited) return;
    if ((s_visible || s_readout_changed) && !gfx_settings_save())
        fprintf(stderr, "[settings] could not save settings while closing the game\n");
    hardware_stats_shutdown();
    vkDeviceWaitIdle(s_dev);
    destroy_targets();
    if (s_pass) { vkDestroyRenderPass(s_dev, s_pass, NULL); s_pass = VK_NULL_HANDLE; }
    if (s_backend_up) { ImGui_ImplVulkan_Shutdown(); s_backend_up = false; }
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    s_inited = false;
}

int ui_visible(void) { return s_visible ? 1 : 0; }

void ui_toggle_readout(void)
{
    g_gfx.show_fps = !g_gfx.show_fps;
    s_readout_changed = true;
    gfx_settings_touch();
}

void ui_toggle(void)
{
    if (!s_inited) return;
    s_visible = !s_visible;
    input_bindings_cancel_capture();
    SDL_SetWindowRelativeMouseMode(s_win, false);
    if (!s_visible && (!gfx_settings_save() || !input_bindings_save()))
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Settings could not be saved",
            "The game could not write pspreco.toml or controls.ini. Move the game to a writable folder and try again.", s_win);
}

int ui_handle_event(const void *sdl_event)
{
    const SDL_Event *e = (const SDL_Event *)sdl_event;
    if (!s_inited || !e) return 0;
    ImGui_ImplSDL3_ProcessEvent(e);
    if (!s_visible && !plane_ui_visible()) return 0;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        return 1;
    default:
        return 0;
    }
}

void ui_note_frame(double frame_ms) { s_frame_ms = frame_ms; }


static void combo_int(const char *label, int *v, const char *const *items, int n)
{
    if (ImGui::BeginCombo(label, items[*v < n ? *v : 0])) {
        for (int i = 0; i < n; i++) {
            bool sel = (*v == i);
            if (ImGui::Selectable(items[i], sel)) { *v = i; gfx_settings_touch(); }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

static void tristate(const char *label, int *v, const char *help)
{
    static const char *k_names[3] = { "Game", "Off", "Force" };
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
    ImGui::SameLine(190.0f);
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(k_names[i], *v == i)) { *v = i; gfx_settings_touch(); }
    }
    ImGui::PopID();
}

static void slider_f(const char *label, float *v, float lo, float hi,
                     const char *fmt, const char *help)
{
    if (ImGui::SliderFloat(label, v, lo, hi, fmt)) gfx_settings_touch();
    if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
}

static void check(const char *label, int *v, const char *help)
{
    bool b = *v != 0;
    if (ImGui::Checkbox(label, &b)) { *v = b; gfx_settings_touch(); }
    if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
}

static void tab_output(void)
{
    static const char *k_aspect[4] = { "Keep 480x272 (letterbox)",
                                       "Integer scale only",
                                       "Stretch to window",
                                       "Force 4:3" };
    static const char *k_filter[2] = { "Nearest (sharp pixels)", "Linear (smooth)" };
    static const char *k_present[3] = { "FIFO (vsync)", "Mailbox (no tearing, no wait)",
                                        "Immediate (tearing allowed)" };
    check("Fullscreen", &g_gfx.fullscreen, "Borderless, at the desktop resolution.");
    if (!g_gfx.fullscreen) {
        if (ImGui::SliderInt("Window scale", &g_gfx.window_scale, 1, 6, "%dx"))
            gfx_settings_touch();
        ImGui::SameLine();
        ImGui::TextDisabled("(%dx%d)", 480 * g_gfx.window_scale,
                            272 * g_gfx.window_scale);
    }
    combo_int("Aspect", &g_gfx.aspect_mode, k_aspect, 4);
    combo_int("Scaling filter", &g_gfx.present_filter, k_filter, 2);
    combo_int("Present mode", &g_gfx.present_mode, k_present, 3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Applied when the swapchain is rebuilt "
                          "(resize the window or toggle fullscreen).");
    if (ImGui::ColorEdit3("Letterbox bars", g_gfx.letterbox)) gfx_settings_touch();

    ImGui::SeparatorText("Frame pacing");
    check("60 FPS gameplay (restart)", &g_gfx.native_60fps,
          "Uses the physics-aware 60 FPS mode. Movies keep their original cadence. "
          "Off selects original 30 FPS after restarting.");
    if (s_advanced) {
    check("Maintain normal game speed", &g_gfx.throttle,
          "Off runs as fast as the host can, which speeds the game up.");
    if (ImGui::SliderInt("Speed", &g_gfx.speed_percent, 25, 800, "%d%%"))
        gfx_settings_touch();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scales the emulated frame time: game logic and audio "
                          "follow it. Needs throttling on.");
    if (ImGui::SliderInt("Frame skip", &g_gfx.frameskip, 0, 5,
                         g_gfx.frameskip ? "%d frames" : "off"))
        gfx_settings_touch();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Renders the GE list but skips presenting, so the "
                          "emulated clock keeps its pace on a slow host.");
    }
    check("Performance overlay (F5)", &g_gfx.show_fps,
          "FPS, frame time, detected CPU/GPU names and total utilization. "
          "GPU usage is for the device rendering this game. Updates once a second; "
          "unavailable counters show N/A.");
    if (s_advanced) { ImGui::SameLine(); check("Show GE counters", &g_gfx.show_stats, NULL); }
}

static void tab_render(void)
{
    static const char *k_texf[3] = { "Game's choice", "Force linear", "Force nearest" };
    if (ImGui::SliderInt("Internal resolution", &g_gfx.res_scale, 1, 4, "%dx"))
        gfx_settings_touch();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Supersamples the GE render targets: %dx%d instead of "
                          "480x272. Costs GPU fill rate, nothing else.",
                          480 * g_gfx.res_scale, 272 * g_gfx.res_scale);
    check("Rain clarity and cockpit water", &g_gfx.rain_clarity,
          "Finer tapered rain streaks and subtle wind-driven water on cockpit glass. Active only when the game draws rain.");
    check("Cinematic clarity", &g_gfx.movie_clarity,
          "Smooth 2x cubic upscale for prerecorded movies from your ISO. "
          "Keeps the original movie cadence; live game text uses the HD font pack.");
    combo_int("Texture filter", &g_gfx.tex_filter, k_texf, 3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Leave this on the game's choice.\n\n"
            "The UI textures pack glyphs edge to edge with no padding, so "
            "forcing linear on a surface the game point-samples pulls the "
            "neighbouring glyph in and leaves a bright line at the seam. "
            "Sharpness comes from internal resolution and mip levels instead; "
            "no preset changes this.");
    if (ImGui::SliderInt("Anisotropy", &g_gfx.anisotropy, 1, 16,
                         g_gfx.anisotropy > 1 ? "%dx" : "off"))
        gfx_settings_touch();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sharpens minified textures at grazing angles. "
                          "Ignored where the driver lacks the feature.");
    check("Mipmaps", &g_gfx.mipmaps_on,
          "Builds the reduced levels the game's textures do not ship. The "
          "menu font is stored at twice its drawn size, so without this the "
          "sampler skips every other texel and thin strokes break up. "
          "Takes effect on textures loaded from now on.");

    ImGui::SeparatorText("Texture pack");
    check("Replacement textures", &g_gfx.tex_replace,
          "Loads a PPSSPP-format pack from textures/ULUS10176 (PSPRECO_TEXTURES "
          "overrides the path): textures.ini plus png, dds or ktx2 files, or a "
          "textures.zip. Takes effect on textures loaded from now on.");
    {
        char st[256];
        texrep_status(st, (int)sizeof st);
        ImGui::TextDisabled("Pack: %s", st);
    }

    ImGui::SeparatorText("Backend");
    check("Render to texture", &g_gfx.rtt,
          "Off keeps every draw in the main target: faster, but effects that "
          "read back a rendered surface break.");
}

static void tab_ge(void)
{
    static const char *k_shade[3] = { "Game's choice", "Flat", "Gouraud" };
    ImGui::TextDisabled("Overrides applied to every primitive the game submits.");
    ImGui::Spacing();
    tristate("Fog", &g_gfx.fog, "Per-vertex, computed on the CPU here.");
    tristate("Lighting", &g_gfx.lighting, "Per-vertex lighting, also CPU-side.");
    tristate("Backface culling", &g_gfx.culling, "Off makes the world see-through.");
    tristate("Alpha test", &g_gfx.alpha_test, NULL);
    tristate("Dithering", &g_gfx.dither, "Only visible in 16-bit colour modes.");
    tristate("Depth test", &g_gfx.depth_test, NULL);
    tristate("Depth write", &g_gfx.depth_write, NULL);
    tristate("Blending", &g_gfx.blending, NULL);
    tristate("Texturing", &g_gfx.texturing, "Off draws flat untextured geometry.");
    ImGui::Spacing();
    combo_int("Shading", &g_gfx.shading, k_shade, 3);
}

static void tab_engine(void)
{
    ImGui::SeparatorText("Atmosphere");
    slider_f("Fog distance", &g_gfx.fog_distance, 0.25f, 4.0f, "%.2fx",
             "Moves the far plane, where haze becomes total. Higher lets you "
             "see further into the distance.");
    slider_f("Haze start", &g_gfx.fog_start, 0.25f, 4.0f, "%.2fx",
             "Moves the near plane, where haze begins. Lower thickens the "
             "air close in; higher clears it.");
    ImGui::TextDisabled("The engine stores fog as a near/far pair, so these "
                        "two move independently.");
    check("Override fog colour", &g_gfx.fog_color_force, NULL);
    if (g_gfx.fog_color_force)
        if (ImGui::ColorEdit3("Fog colour", g_gfx.fog_color)) gfx_settings_touch();

    ImGui::SeparatorText("Cut geometry");
    check("Skip lines", &g_gfx.skip_lines, NULL);
    ImGui::SameLine();
    check("Skip points", &g_gfx.skip_points, NULL);
}

static void tab_colour(void)
{
    slider_f("Brightness", &g_gfx.brightness, -0.5f, 0.5f, "%.2f", NULL);
    slider_f("Contrast", &g_gfx.contrast, 0.5f, 2.0f, "%.2f", NULL);
    slider_f("Saturation", &g_gfx.saturation, 0.0f, 2.0f, "%.2f",
             "0 is greyscale.");
    slider_f("Gamma", &g_gfx.gamma, 0.5f, 2.5f, "%.2f", NULL);
    ImGui::Spacing();
    if (ImGui::Button("Neutral")) {
        g_gfx.brightness = 0.0f;
        g_gfx.contrast = g_gfx.saturation = g_gfx.gamma = 1.0f;
        gfx_settings_touch();
    }
    ImGui::TextDisabled("Applied while the frame is converted for display, so "
                        "screenshots and frame dumps stay untouched.");
}

static void tab_input(void)
{
    if (ImGui::BeginCombo("Input device", present_controller_name())) {
        int n = 0;
        SDL_JoystickID *ids = SDL_GetJoysticks(&n);
        for (int i = 0; ids && i < n; ++i) {
            ImGui::PushID((int)ids[i]);
            if (ImGui::Selectable(SDL_GetJoystickNameForID(ids[i]))) present_controller_select(ids[i]);
            ImGui::PopID();
        }
        SDL_free(ids);
        ImGui::EndCombo();
    }
    ImGui::TextWrapped("Controller: %s", present_controller_name());
    check("Controller vibration", &g_gfx.rumble,
          "Gun pulses, missile/special launches, damage impacts and low-speed buffet (700 km/h: 80%, 350 km/h: 100%). Stops in menus and on focus loss. 100% uses full motor output.");
    if (ImGui::SliderInt("Vibration strength", &g_gfx.rumble_strength, 0, 100, "%d%%")) gfx_settings_touch();
    const bool has_rumble = present_rumble_supported() != 0;
    ImGui::BeginDisabled(!has_rumble || !g_gfx.rumble || g_gfx.rumble_strength == 0);
    if (ImGui::Button("Test vibration")) present_rumble_test();
    ImGui::EndDisabled();
    if (!has_rumble) ImGui::TextDisabled("Vibration unavailable on the selected device/connection.");
    ImGui::TextDisabled("Xbox / PlayStation / Switch / mapped USB pads\n"
                        "Left stick flies; right stick looks.\n"
                        "Menu/Options/+ pauses; View/Create/- changes radar.");
    ImGui::Spacing();
    check("Right stick moves the camera", &g_gfx.rstick_cam,
          "Uses the engine's own look limits: 170 degrees in the chase views, "
          "80 in the cockpit, which never looks above the horizon. The "
          "scripted and replay cameras are left alone.");
    if (g_gfx.rstick_cam) {
        ImGui::Indent();
        if (ImGui::SliderInt("Look sensitivity", &g_gfx.rstick_sens,
                             10, 400, "%d%%"))
            gfx_settings_touch();
        check("Invert look", &g_gfx.rstick_invert, NULL);
        ImGui::Unindent();
    }
    ImGui::Spacing();
    check("Modern shoulder controls", &g_gfx.modern_controls,
          "LB/RB, L1/R1 or L/R yaw. LT/RT, L2/R2 or ZL/ZR brake/accelerate. "
          "Keyboard Q/E also yaw with this enabled. Weapons stay unchanged. "
          "Menus retain PSP shoulder navigation. Disable for the original "
          "PSP shoulder mapping.");
    check("R3 changes view", &g_gfx.r3_view,
          "Cycles the same three views, in the same order, as the game's own "
          "view button.");
    if (ImGui::SliderInt("Controller dead zone", &g_gfx.input_deadzone, 0, 50, "%d%%")) gfx_settings_touch();
    ImGui::Spacing();
    ImGui::SeparatorText("Keyboard");
    check("Modern keyboard (Zero-style)", &g_gfx.modern_keyboard,
          "Separate flight/menu keys. Escape pauses instead of quitting. "
          "Disable to restore the previous X/Z/A/S keyboard layout.");
    if (g_gfx.modern_keyboard) {
        ImGui::TextWrapped("Flight: W/S pitch down/up; A/D roll; Q/E yaw.\n"
                          "Ctrl brake; Space accelerate.\n"
                          "J gun; L missile/special; K weapon; T target.\n"
                          "V view; M or Tab radar; Esc pause; F1 Start/skip.\n"
                          "Camera: numpad 4/6/8/2 or hold Alt + WASD.\n"
                          "Menus: WASD/arrows navigate; Enter/Space confirm;\n"
                          "Backspace/Esc back; Q/E tabs. Alt+F4 exits.");
    } else {
        ImGui::TextWrapped("Legacy: X gun, Z missile, A weapon, S target;\n"
                          "Q/W brake/accelerate; arrows D-pad; IJKL camera;\n"
                          "C view; Enter Start; Shift radar; Esc exits.");
    }
    ImGui::SeparatorText("Mouse");
    check("Mouse flight control", &g_gfx.mouse_flight,
          "Move the mouse to pitch/roll. Hold Alt for camera look. "
          "The pointer is captured only during flight; F4 releases it.");
    if (ImGui::SliderInt("Mouse sensitivity", &g_gfx.mouse_sensitivity, 10, 400, "%d%%")) gfx_settings_touch();
    ImGui::TextWrapped("Left click: gun; right click: missile; middle click: target; "
                      "wheel: weapon. Mouse buttons use the modern keyboard preset.");
    if (ImGui::CollapsingHeader("Rebind flight controls")) {
        ImGui::TextWrapped("Choose a binding, then press a key/button, move an axis or hat. "
                          "Escape cancels. Keyboard/menu navigation stays available. "
                          "For an unmapped joystick/HOTAS, select the device above and bind its actions here. "
                          "Controller profiles follow the device GUID; keyboard/mouse are shared.");
        ImGui::TextWrapped("Connected devices combine: separate USB stick, throttle and pedals can work together. "
                          "Pitch/roll/rudder axes are proportional. Throttle axes drive ACX's original brake/accelerate actions. "
                          "Use Full range for a throttle lever; positive/negative halves for a centered axis.");
        if (input_bindings_capturing()) ImGui::TextColored(ImVec4(1,.8f,.2f,1), "Waiting for input... Esc cancels");
        static int src = IB_KEYBOARD;
        const char *sources[] = {"Keyboard", "Mouse buttons", "Controller / joystick"};
        ImGui::Combo("Rebind device", &src, sources, 3);
        if (ImGui::BeginTable("bindings", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Action"); ImGui::TableSetupColumn("Binding");
            ImGui::TableHeadersRow();
            for (int a = 0; a < IA_COUNT; ++a) {
                ImGui::PushID(a); ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(input_action_name(a));
                {
                    char label[80]; ImGui::PushID(src); ImGui::TableSetColumnIndex(1);
                    input_binding_label(src, a, label, sizeof label);
                    if (ImGui::Button(label)) input_bindings_capture(src, a);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) input_binding_set(src, a, {IB_NONE,0,0});
                    InputBinding b = input_binding_get(src, a);
                    if (src == IB_CONTROLLER && b.kind == IB_AXIS) {
                        const char *modes[] = {"Positive half", "Negative half", "Full range", "Full reversed"};
                        int mode = b.direction == 1 ? 0 : b.direction == -1 ? 1 : b.direction == 2 ? 2 : 3;
                        ImGui::SetNextItemWidth(115);
                        if (ImGui::Combo("##axis", &mode, modes, 4)) {
                            const int dirs[] = {1,-1,2,-2}; b.direction = dirs[mode]; input_binding_set(src, a, b);
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Reset keyboard")) input_bindings_reset(IB_KEYBOARD);
        ImGui::SameLine(); if (ImGui::Button("Reset mouse")) input_bindings_reset(IB_MOUSE_DEVICE);
        ImGui::SameLine(); if (ImGui::Button("Reset controller")) input_bindings_reset(IB_CONTROLLER);
        ImGui::TextWrapped("Bindings save to controls.ini when F4 closes. Copy that file with pspreco.toml to retain your layout.");
    }
}

static void tab_debug(void)
{
    check("GE counters to the log", &g_gfx.ge_stats_log, NULL);
    check("GPU backend logging", &g_gfx.ge_cmd_log, NULL);
    check("Dump textures", &g_gfx.tex_dump,
          "Writes every texture the game uploads to textures/ULUS10176/new as "
          "<hash>.png, skipping any the pack already replaces. Move them up one "
          "directory and they are picked up without an ini entry.");
    if (ImGui::SliderInt("Frame dump interval", &g_gfx.frame_dump_every, 0, 600,
                         g_gfx.frame_dump_every ? "every %d frames" : "off"))
        gfx_settings_touch();
    ImGui::Spacing();
    ImGui::SeparatorText("This session");
    ImGui::Text("%.1f fps  (%.2f ms/frame)", s_fps, s_frame_ms);
    ImGui::Text("GE primitives: %lu", g_ge_prim_count);
    ImGui::Text("Settings file: %s", gfx_settings_path());
}

static void draw_menu(void)
{
    ImGui::SetNextWindowSize(ImVec2(660, 480), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(30, 24), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings  (F4 closes)", NULL, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    int cur = gfx_preset_current();
    ImGui::TextUnformatted("Preset:");
    for (int i = 0; i < 4; i++) {
        ImGui::SameLine();
        bool on = (cur == i);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(gfx_preset_name(i), ImVec2(78, 0))) gfx_preset_apply(i);
        if (on) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", cur < 0 ? "(custom)" : "");

    ImGui::Checkbox("Advanced settings", &s_advanced);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Output"))  { tab_output(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Renderer")){ tab_render(); ImGui::EndTabItem(); }
        if (s_advanced && ImGui::BeginTabItem("GE"))      { tab_ge();     ImGui::EndTabItem(); }
        if (s_advanced && ImGui::BeginTabItem("Engine"))  { tab_engine(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Colour"))  { tab_colour(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Input"))   { tab_input();  ImGui::EndTabItem(); }
        if (s_advanced && ImGui::BeginTabItem("Debug"))   { tab_debug();  ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button("Save now")) {
        s_saved = gfx_settings_save() && input_bindings_save();
        s_saved_until = SDL_GetTicks() + 3000;
    }
    if (s_saved_until > SDL_GetTicks()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(s_saved ? "Saved" : "Could not save settings");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) { gfx_settings_load(); input_bindings_reload(); }
    ImGui::SameLine();
    if (ImGui::Button("Defaults"))   gfx_settings_defaults();
    ImGui::SameLine();
    ImGui::TextDisabled("saved to %s on close", gfx_settings_path());
    ImGui::End();
}

static void draw_cockpit_water(void)
{
    if(!g_gfx.rain_clarity || !g_ram || !ge_weather_rain_active()
        || s_visible || plane_ui_visible() || !acx_flight_active(g_ram)
        || mem_r8(g_ram,0x08a80c36u)!=1) return;
    ImVec2 size=ImGui::GetIO().DisplaySize;
    float w=size.x,h=size.y,x0=0,y0=0;
    if(w<=0 || h<=0) return;
    if(g_gfx.aspect_mode==ASPECT_INTEGER) {
        float integer=floorf(fminf(w/480.0f,h/272.0f));if(integer<1)integer=1;
        w=480*integer;h=272*integer;x0=(size.x-w)*0.5f;y0=(size.y-h)*0.5f;
    } else if(g_gfx.aspect_mode!=ASPECT_STRETCH) {
        float ratio=g_gfx.aspect_mode==ASPECT_4_3 ? 4.0f/3.0f : 480.0f/272.0f;
        if(w/h>ratio){w=h*ratio;x0=(size.x-w)*0.5f;}else{h=w/ratio;y0=(size.y-h)*0.5f;}
    }
    ImDrawList *d=ImGui::GetBackgroundDrawList();
    d->PushClipRect(ImVec2(x0,y0),ImVec2(x0+w,y0+h),true);
    float time=(float)(SDL_GetTicks()%600000u)*0.001f;
    float scale=w/960.0f;
    for(unsigned i=0;i<20;i++) {
        unsigned seed=(i+1u)*2654435761u;
        float cycle=3.0f+(float)(seed%400u)*0.01f;
        float p=fmodf(time/cycle+(float)((seed>>12)%1000u)*0.001f,1.0f);
        float fade=fminf(1.0f,fminf(p*8.0f,(1.0f-p)*6.0f));
        /* Confine the overlay to the common forward windshield area, away
         * from opaque side panels, the central reticle and HUD scales. */
        float u=0.38f+(float)((seed>>4)%240u)*0.001f;
        float v=0.29f+(float)((seed>>20)%320u)*0.001f-p*0.025f;
        float cx=u-0.5f,cy=v-0.5f;if(cx*cx+cy*cy<0.0045f)continue;
        float x=x0+(u+(u-0.5f)*p*0.015f)*w,y=y0+v*h;
        float radius=(1.1f+(float)(seed%120u)*0.01f)*scale;
        float trail=(8.0f+(float)((seed>>9)%16u))*scale;
        float tx=(u-0.5f)*trail*0.5f;
        d->AddLine(ImVec2(x-tx,y+trail),ImVec2(x,y),IM_COL32(12,22,28,(int)(21*fade)),2.3f*scale);
        d->AddLine(ImVec2(x-tx+scale,y+trail),ImVec2(x+scale,y),IM_COL32(215,231,235,(int)(23*fade)),0.75f*scale);
        d->AddCircleFilled(ImVec2(x,y),radius,IM_COL32(15,26,32,(int)(35*fade)),12);
        d->AddCircle(ImVec2(x,y),radius,IM_COL32(200,222,229,(int)(39*fade)),12,0.7f*scale);
        d->AddCircleFilled(ImVec2(x-radius*0.3f,y-radius*0.4f),radius*0.3f,IM_COL32(232,242,245,(int)(50*fade)),8);
    }
    d->PopClipRect();
}

static void draw_readout(void)
{
    if (!g_gfx.show_fps) return;
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
    float width = ImGui::GetIO().DisplaySize.x - 32.0f;
    if (width < 160.0f) width = 160.0f;
    if (width > 720.0f) width = 720.0f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(width + 16, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.35f);
    if (ImGui::Begin("readout", NULL,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
                     | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav
                     | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Text("Game %.1f FPS | Display %.1f FPS | %.2f ms",
                    s_game_fps, s_fps, s_frame_ms);
        HardwareStats hw = hardware_stats_read();
        char cpu[24] = "N/A", gpu[24] = "N/A";
        if (hw.cpu_valid) snprintf(cpu, sizeof cpu, "%.0f%%", hw.cpu_percent);
        if (hw.gpu_valid) snprintf(gpu, sizeof gpu, "%.0f%%", hw.gpu_percent);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextWrapped("%s  %s", hw.cpu_name, cpu);
        ImGui::TextWrapped("%s  %s", hw.gpu_name, gpu);
        ImGui::PopTextWrapPos();
        if (g_gfx.show_stats) {
            ImGui::Text("%dx%d  x%d", 480 * g_gfx.res_scale, 272 * g_gfx.res_scale,
                        g_gfx.res_scale);
            ImGui::Text("prims %lu", g_ge_prim_count);
        }
    }
    ImGui::End();
}

void ui_new_frame(void)
{
    if (!s_inited || !s_backend_up) return;
    hardware_stats_set_visible(g_gfx.show_fps != 0);

    uint64_t now = SDL_GetTicks();
    s_frames++;
    if (now - s_last_ticks >= 500) {
        unsigned g = display_game_frames();
        s_fps = s_frames * 1000.0 / (double)(now - s_last_ticks);
        s_frame_ms = (double)(now - s_last_ticks) / (s_frames ? s_frames : 1);
        s_game_fps = (g - s_last_game_frames) * 1000.0
                   / (double)(now - s_last_ticks);
        s_last_game_frames = g;
        s_frames = 0;
        s_last_ticks = now;
    }

    ImGui_ImplVulkan_NewFrame();
    if (input_bindings_block_navigation()) ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    else ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    draw_cockpit_water();
    if (s_visible) draw_menu();
    plane_ui_draw();
    draw_readout();
    ImGui::Render();
}

int ui_render(void *cmd, uint32_t index)
{
    VkRenderPassBeginInfo rbi;
    ImDrawData *dd;

    if (!s_inited || !s_backend_up || index >= s_n_img || !s_fbs[index]) return 0;
    dd = ImGui::GetDrawData();
    if (!dd) return 0;

    memset(&rbi, 0, sizeof rbi);
    rbi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass        = s_pass;
    rbi.framebuffer       = s_fbs[index];
    rbi.renderArea.extent = s_ext;
    vkCmdBeginRenderPass((VkCommandBuffer)cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(dd, (VkCommandBuffer)cmd);
    vkCmdEndRenderPass((VkCommandBuffer)cmd);
    return 1;
}
