#ifdef PSPRECO_SDL3

#include "pspreco.h"
#include "ge.h"
#include "ge_shared.h"
#include "tex_replace.h"
#include "../host/present.h"
#include "../host/settings.h"
#include "../host/video_clarity.h"

extern u8 *g_ram;
#define SR_HOST(a) (g_ram + ((a) & PSP_ADDR_MASK))

int  gegpu_init(void);
void gegpu_shutdown(void);
static void gegpu_flush(const char *reason);
static int  gegpu_present(uint32_t fbaddr, int fmt, uint32_t stride);

#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define FB_W 512
#define FB_H 272
#define DRAW_W 480
#define MAX_VERTS  131072
#define MAX_BATCH  4096
#define MAX_PIPES  192
#define MAX_TEX    1024
#define MAX_TGT    8
#define MAX_DEP    4
#define CLUT_MAX_LOAD_BYTES 2048

static const uint32_t k_vert_spv[] =
#include "psp_vert.inc"
;
static const uint32_t k_frag_spv[] =
#include "psp_frag.inc"
;

typedef struct { float x, y, z, rw, u, v, fog; uint32_t rgba;
                 float ulo, vlo, uhi, vhi; } GpuVert;

typedef struct { int32_t cfg[4]; float texenv[4]; float fogcol[4]; float texsize[4]; } PushPC;
enum { F_TEX = 1, F_RGBA = 2, F_DBL = 4, F_FOG = 8, F_PERSP = 16, F_CLEAR = 32,
       F_NEAREST = 64, F_SA2X = 128, F_SA2XI = 256 };

typedef struct {
    uint8_t blend_on, srcf, dstf, eq;
    uint8_t cull;
    uint8_t ztest, zwrite, zfunc;
    uint8_t cmask;
} PipeKey;

typedef struct {
    PipeKey key;
    int32_t sx, sy, sw, sh;
    float   bconst[4];
    PushPC  pc;
    VkDescriptorSet dset;
    uint32_t first, count;
} Batch;

typedef struct {
    uint64_t key, hash;
    VkImage img; VkDeviceMemory mem; VkImageView view; VkSampler smp;
    VkDescriptorSet set;
    int w, h;
    uint32_t levels;
    int pending;
    uint64_t lru;
    int linear, clamp_u, clamp_v;
    float half_u, half_v;
    uint64_t bytes;
} TexEnt;

typedef struct { const uint32_t *px; int w, h; } TexLevel;

typedef struct { PipeKey key; VkPipeline pipe; int used; } PipeEnt;

typedef struct {
    int used;
    uint32_t zba, zstride;
    VkImage img; VkDeviceMemory mem; VkImageView view;
    VkImageLayout layout;
    uint32_t scale;
} DepthEnt;

typedef struct {
    int used;
    uint32_t fba, stride, fmt;
    VkImage img; VkDeviceMemory mem; VkImageView view;
    VkImageLayout layout;
    VkDescriptorSet set_n, set_l;
    DepthEnt *dep;
    VkFramebuffer fb;
    int gpu_valid;
    uint64_t lru;
    uint64_t render_gen;
    uint64_t clean_gen;
    uint64_t part_gen;
    uint32_t part_y0, part_y1;
    uint32_t drawn_y0, drawn_y1;
    uint32_t scale;
    VkImage rimg; VkDeviceMemory rmem;
    VkImageLayout rlayout;
} Target;

static uint32_t s_scale = 1;
static unsigned s_scale_gen;

static uint32_t gpu_scale(void) {
    if (s_scale_gen != g_gfx_generation) {
        uint32_t want = (uint32_t)(g_gfx.res_scale < 1 ? 1
                                 : g_gfx.res_scale > 4 ? 4 : g_gfx.res_scale);
        s_scale_gen = g_gfx_generation;
        s_scale = want;
    }
    return s_scale;
}

static int s_ready = 0;
extern float g_vk_max_aniso;
#define s_max_aniso g_vk_max_aniso
static GeState  *s_ge;
static uint16_t *s_zbuf;

static VkPhysicalDevice s_pdev;
static VkDevice   s_dev;
static VkQueue    s_queue;
static VkCommandPool s_pool;

#define RING 3
typedef struct { VkCommandBuffer cmd; VkFence fence; } RingSlot;
static RingSlot  s_ring[RING];
static uint64_t  s_submit_id;
static uint64_t  s_completed_id;
static VkCommandBuffer s_cmd;

static VkRenderPass s_rp;

static VkBuffer s_vbuf;
static VkDeviceMemory s_vbuf_m;
static GpuVert *s_vmap;
static VkDeviceSize s_voff;
static GpuVert *s_vstage;

#define XFER_RING 3
#define XFER_SIZE (1u << 20)
typedef struct { VkBuffer buf; VkDeviceMemory mem; void *map; uint64_t id; } XferSlot;
static XferSlot s_xfer[XFER_RING];
static int      s_xfer_i;

static VkDescriptorSetLayout s_dlayout;
static VkDescriptorPool s_dpool_tex, s_dpool_fix;
static VkPipelineLayout s_playout;
static VkShaderModule s_vs, s_fs;

static TexEnt  s_tex[MAX_TEX];
static int     s_tex_n = 0;
static VkImage s_white; static VkDeviceMemory s_white_mem;
static VkImageView s_white_view; static VkSampler s_white_smp;
static VkDescriptorSet s_white_set;

static VkImage s_snapimg; static VkDeviceMemory s_snapimg_mem;
static VkImageView s_snap_view; static VkImageLayout s_snap_layout;
static VkSampler s_smp_n, s_smp_l;
static VkDescriptorSet s_snap_n, s_snap_l;
static Target *s_snap_src = NULL;
static uint64_t s_snap_srcgen = 0;

static PipeEnt s_pipes[MAX_PIPES];
static int s_pipe_n = 0;

static Target   s_tgts[MAX_TGT];
static void target_drew(Target *t, uint32_t y0, uint32_t y1);
static DepthEnt s_deps[MAX_DEP];
static Target  *s_cur = NULL;
static uint64_t s_lru = 1;

static Batch    s_batch[MAX_BATCH];
static uint32_t s_nbatch = 0, s_nverts = 0;

static int s_log = 0;
static unsigned long s_cnt_submit = 0, s_cnt_tri = 0, s_cnt_spr = 0, s_cnt_line = 0;
static unsigned long s_cnt_present_gpu = 0, s_cnt_present_cpu = 0, s_cnt_rtt = 0;
static unsigned long s_cnt_readback = 0, s_cnt_upload = 0, s_cnt_dirty = 0;
static unsigned long s_cnt_readback_rows = 0, s_cnt_readback_skip = 0;
enum { RB_CLUT = 0, RB_XFERSRC, RB_RTT, RB_REACQUIRE, RB_PRESENT, RB_REASONS };
static const char *const k_rb_reason_name[RB_REASONS] = {
    "clut", "xfersrc", "rtt", "reacq", "present"
};
static unsigned long s_cnt_rb_by_reason[RB_REASONS];
static unsigned long s_cnt_rb_rows_by_reason[RB_REASONS];
static int s_rb_reason = RB_CLUT;
static unsigned long s_cnt_snap = 0, s_cnt_texup = 0, s_cnt_xferblit = 0;
static unsigned long s_cnt_repl = 0;
static uint64_t s_tex_bytes = 0;

static uint32_t s_texscratch[512 * 512];
static uint32_t s_pxscratch[FB_W * FB_H];

#define VKC(expr) do { VkResult vr_ = (expr); if (vr_ != VK_SUCCESS) { \
    fprintf(stderr, "gegpu: %s failed: %d\n", #expr, (int)vr_); return 0; } } while (0)

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(s_pdev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf,
                       VkDeviceMemory *mem, void **map) {
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size; bci.usage = usage;
    VKC(vkCreateBuffer(s_dev, &bci, NULL, buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(s_dev, *buf, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKC(vkAllocateMemory(s_dev, &mai, NULL, mem));
    VKC(vkBindBufferMemory(s_dev, *buf, *mem, 0));
    if (map) VKC(vkMapMemory(s_dev, *mem, 0, VK_WHOLE_SIZE, 0, map));
    return 1;
}

static int make_image_mip(uint32_t w, uint32_t h, uint32_t levels, VkFormat fmt,
                          VkImageUsageFlags usage, VkImage *img, VkDeviceMemory *mem) {
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
    ici.extent.width = w; ici.extent.height = h; ici.extent.depth = 1;
    ici.mipLevels = levels; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(s_dev, &ici, NULL, img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(s_dev, *img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKC(vkAllocateMemory(s_dev, &mai, NULL, mem));
    VKC(vkBindImageMemory(s_dev, *img, *mem, 0));
    return 1;
}

static int make_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                      VkImage *img, VkDeviceMemory *mem) {
    return make_image_mip(w, h, 1, fmt, usage, img, mem);
}

static int make_view_mip(VkImage img, VkFormat fmt, VkImageAspectFlags aspect,
                         uint32_t levels, VkImageView *view) {
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange.aspectMask = aspect;
    vci.subresourceRange.levelCount = levels; vci.subresourceRange.layerCount = 1;
    VKC(vkCreateImageView(s_dev, &vci, NULL, view));
    return 1;
}

static int make_view(VkImage img, VkFormat fmt, VkImageAspectFlags aspect, VkImageView *view) {
    return make_view_mip(img, fmt, aspect, 1, view);
}

static uint32_t mip_levels_for(int w, int h) {
    uint32_t n = 1, d = (uint32_t)(w > h ? w : h);
    while (d > 1) { d >>= 1; n++; }
    return n;
}

static void layout_scope(VkImageLayout layout, int is_src,
                         VkPipelineStageFlags *stage, VkAccessFlags *access) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = is_src ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         : (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                            | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        return;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        *stage = is_src ? VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                        : VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        *access = is_src ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                         : (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                            | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        return;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        *access = VK_ACCESS_SHADER_READ_BIT;
        return;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *access = VK_ACCESS_TRANSFER_READ_BIT;
        return;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *access = VK_ACCESS_TRANSFER_WRITE_BIT;
        return;
    default:
        *stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        *access = is_src ? VK_ACCESS_MEMORY_WRITE_BIT
                         : (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        return;
    }
}

static void to_layout(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                      VkImageLayout *cur, VkImageLayout want) {
    if (*cur == want) return;
    VkPipelineStageFlags src_stage, dst_stage;
    VkAccessFlags src_access, dst_access;
    layout_scope(*cur, 1, &src_stage, &src_access);
    layout_scope(want, 0, &dst_stage, &dst_access);
    VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    mb.srcAccessMask = src_access;
    mb.dstAccessMask = dst_access;
    mb.oldLayout = *cur; mb.newLayout = want;
    mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = img;
    mb.subresourceRange.aspectMask = aspect;
    mb.subresourceRange.levelCount = 1;
    mb.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage,
                         0, 0, NULL, 0, NULL, 1, &mb);
    *cur = want;
}

extern unsigned long long g_prof_wait_tk;
extern unsigned long long g_prof_tex_tk;
extern unsigned long long g_prof_rback_tk;
extern unsigned long g_prof_wait_n;
extern unsigned long g_prof_on;
u64 ge_prof_now_tk(void);

static int gpu_sync(void);

static int gpu_wait_id(uint64_t id) {
    if (id > s_submit_id) id = s_submit_id;
    if (id <= s_completed_id) return 1;
    u64 t0 = g_prof_on ? ge_prof_now_tk() : 0;
    while (s_completed_id < id) {
        VkFence f = s_ring[(s_completed_id + 1) % RING].fence;
        VKC(vkWaitForFences(s_dev, 1, &f, VK_TRUE, UINT64_MAX));
        VKC(vkResetFences(s_dev, 1, &f));
        s_completed_id++;
    }
    if (g_prof_on) { g_prof_wait_tk += ge_prof_now_tk() - t0; g_prof_wait_n++; }
    return 1;
}

static int gpu_sync(void) { return gpu_wait_id(s_submit_id); }

static int cmd_begin(void) {
    uint64_t next = s_submit_id + 1;
    if (next > RING) gpu_wait_id(next - RING);
    s_cmd = s_ring[next % RING].cmd;
    vkResetCommandBuffer(s_cmd, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(s_cmd, &bi));
    return 1;
}

static int cmd_submit(void) {
    VKC(vkEndCommandBuffer(s_cmd));
    uint64_t id = s_submit_id + 1;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &s_cmd;
    VKC(vkQueueSubmit(s_queue, 1, &si, s_ring[id % RING].fence));
    s_submit_id = id;
    static int serial = -1;
    if (serial < 0) { const char *sv = getenv("SR_GPU_SERIAL");
                      serial = (sv && sv[0] && strcmp(sv, "0") != 0) ? 1 : 0; }
    return serial ? gpu_sync() : 1;
}

static XferSlot *xfer_acquire(void) {
    XferSlot *x = &s_xfer[s_xfer_i];
    s_xfer_i = (s_xfer_i + 1) % XFER_RING;
    gpu_wait_id(x->id);
    return x;
}
static void xfer_used(XferSlot *x) { x->id = s_submit_id; }

static uint32_t fb_unpack(uint32_t raw, uint32_t fmt) {
    uint32_t r, g, b, a;
    switch (fmt & 3) {
        case 0: r=(raw&0x1F)*255/31; g=((raw>>5)&0x3F)*255/63; b=((raw>>11)&0x1F)*255/31; a=255; break;
        case 1: r=(raw&0x1F)*255/31; g=((raw>>5)&0x1F)*255/31; b=((raw>>10)&0x1F)*255/31; a=(raw&0x8000)?255:0; break;
        case 2: r=(raw&0xF)*17; g=((raw>>4)&0xF)*17; b=((raw>>8)&0xF)*17; a=((raw>>12)&0xF)*17; break;
        default: return raw;
    }
    return r | (g << 8) | (b << 16) | (a << 24);
}

static VkPipeline pipe_create(const PipeKey *k) {
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = s_vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = s_fs; st[1].pName = "main";

    VkVertexInputBindingDescription bind = { 0, sizeof(GpuVert), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription at[5] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT,       16 },
        { 2, 0, VK_FORMAT_R32_SFLOAT,          24 },
        { 3, 0, VK_FORMAT_R8G8B8A8_UNORM,      28 },
        { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32 },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 1;   vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 5; vi.pVertexAttributeDescriptions = at;

    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = k->cull;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = k->ztest;
    ds.depthWriteEnable = k->zwrite;
    ds.depthCompareOp = (VkCompareOp)k->zfunc;

    VkPipelineColorBlendAttachmentState ba = {0};
    ba.blendEnable = k->blend_on;
    ba.srcColorBlendFactor = (VkBlendFactor)k->srcf;
    ba.dstColorBlendFactor = (VkBlendFactor)k->dstf;
    ba.colorBlendOp = (VkBlendOp)k->eq;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask = k->cmask;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                             VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dn.dynamicStateCount = 3; dn.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.stageCount = 2; pci.pStages = st;
    pci.pVertexInputState = &vi; pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp; pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb; pci.pDynamicState = &dn;
    pci.layout = s_playout; pci.renderPass = s_rp;

    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &pci, NULL, &p);
    if (r != VK_SUCCESS) fprintf(stderr, "gegpu: pipeline create failed: %d\n", (int)r);
    return p;
}

static VkPipeline pipe_get(const PipeKey *k) {
    for (int i = 0; i < s_pipe_n; i++)
        if (!memcmp(&s_pipes[i].key, k, sizeof(*k))) return s_pipes[i].pipe;
    if (s_pipe_n >= MAX_PIPES) return VK_NULL_HANDLE;
    VkPipeline p = pipe_create(k);
    if (!p) return VK_NULL_HANDLE;
    s_pipes[s_pipe_n].key = *k; s_pipes[s_pipe_n].pipe = p; s_pipe_n++;
    return p;
}

static VkDescriptorSet make_descriptor(VkImageView view, VkSampler smp, VkDescriptorPool pool) {
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &s_dlayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(s_dev, &dai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorImageInfo dii = { smp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = set; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &dii;
    vkUpdateDescriptorSets(s_dev, 1, &wr, 0, NULL);
    return set;
}

static uint32_t s_flushgen = 1;

static void batches_reset(void) {
    s_nbatch = 0; s_nverts = 0;
    s_flushgen++;
    for (int i = 0; i < s_tex_n; i++) s_tex[i].pending = 0;
}

static int submit_pending(void) {
    if (!s_nbatch || !s_cur) { if (s_nbatch == 0) batches_reset(); return 1; }
    Target *t = s_cur;
    if (!cmd_begin()) { batches_reset(); return 0; }

    s_voff = (VkDeviceSize)((s_submit_id + 1) % RING) * MAX_VERTS * sizeof(GpuVert);
    if (s_nverts)
        memcpy((u8 *)s_vmap + s_voff, s_vstage, (size_t)s_nverts * sizeof(GpuVert));

    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    to_layout(s_cmd, t->dep->img, VK_IMAGE_ASPECT_DEPTH_BIT, &t->dep->layout,
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    const uint32_t sc_mul = t->scale;
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = s_rp; rbi.framebuffer = t->fb;
    rbi.renderArea.extent.width = FB_W * sc_mul;
    rbi.renderArea.extent.height = FB_H * sc_mul;
    vkCmdBeginRenderPass(s_cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp = { 0, 0, (float)(FB_W * sc_mul), (float)(FB_H * sc_mul), 0.0f, 1.0f };
    vkCmdSetViewport(s_cmd, 0, 1, &vp);
    vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_vbuf, &s_voff);

    VkPipeline cur = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < s_nbatch; i++) {
        Batch *b = &s_batch[i];
        VkPipeline p = pipe_get(&b->key);
        if (!p) continue;
        if (p != cur) { vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p); cur = p; }
        VkRect2D sc = { { b->sx * (int32_t)sc_mul, b->sy * (int32_t)sc_mul },
                        { (uint32_t)b->sw * sc_mul, (uint32_t)b->sh * sc_mul } };
        vkCmdSetScissor(s_cmd, 0, 1, &sc);
        target_drew(t, (uint32_t)(b->sy < 0 ? 0 : b->sy),
                    (uint32_t)(b->sy < 0 ? 0 : b->sy) + (uint32_t)(b->sh < 0 ? 0 : b->sh));
        vkCmdSetBlendConstants(s_cmd, b->bconst);
        vkCmdPushConstants(s_cmd, s_playout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(b->pc), &b->pc);
        vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 0, 1, &b->dset, 0, NULL);
        vkCmdDraw(s_cmd, b->count, 1, b->first, 0);
    }
    vkCmdEndRenderPass(s_cmd);
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    int ok = cmd_submit();
    t->gpu_valid = 1;
    t->render_gen++;
    s_cnt_submit++;
    if (s_log)
        fprintf(stderr, "GEGPU submit #%lu batches=%u verts=%u tgt=0x%08x/%u fmt=%u\n",
                s_cnt_submit, s_nbatch, s_nverts, t->fba, t->stride, t->fmt);
    batches_reset();
    return ok;
}

static void stats_tick(void) {
    static unsigned long last_ms = 0;
    unsigned long now = (unsigned long)(clock() * 1000ull / CLOCKS_PER_SEC);
    if (now - last_ms < 5000) return;
    last_ms = now;
    fprintf(stderr, "GEGPU stats: submits=%lu tris=%lu spr=%lu lines=%lu rtt=%lu snap=%lu "
            "present[gpu=%lu cpu=%lu] upload=%lu readback=%lu[rows=%lu skip=%lu] "
            "texup=%lu dirty=%lu xferblit=%lu pipes=%d texs=%d repl=%lu texmb=%lu\n",
            s_cnt_submit, s_cnt_tri, s_cnt_spr, s_cnt_line, s_cnt_rtt, s_cnt_snap,
            s_cnt_present_gpu, s_cnt_present_cpu, s_cnt_upload, s_cnt_readback,
            s_cnt_readback_rows, s_cnt_readback_skip, s_cnt_texup,
            s_cnt_dirty, s_cnt_xferblit, s_pipe_n, s_tex_n,
            s_cnt_repl, (unsigned long)(s_tex_bytes >> 20));
    fprintf(stderr, "GEGPU rback:");
    for (int i = 0; i < RB_REASONS; i++)
        fprintf(stderr, " %s=%lu/%lurows", k_rb_reason_name[i],
                s_cnt_rb_by_reason[i], s_cnt_rb_rows_by_reason[i]);
    fputc('\n', stderr);
}

static void target_drew(Target *t, uint32_t y0, uint32_t y1) {
    if (y1 > FB_H) y1 = FB_H;
    if (y0 >= y1) return;
    if (t->drawn_y0 >= t->drawn_y1) { t->drawn_y0 = y0; t->drawn_y1 = y1; return; }
    if (y0 < t->drawn_y0) t->drawn_y0 = y0;
    if (y1 > t->drawn_y1) t->drawn_y1 = y1;
}

static int target_rows_for_range(const Target *t, uint32_t a0, uint32_t bytes,
                                 uint32_t *out_y0, uint32_t *out_y1) {
    uint32_t bpp   = t->fmt == 3 ? 4 : 2;
    uint32_t pitch = t->stride * bpp;
    uint32_t draw  = (t->stride < DRAW_W ? t->stride : DRAW_W) * bpp;
    if (!pitch || !bytes) return 0;
    if (a0 + bytes <= t->fba || t->fba + pitch * FB_H <= a0) return 0;
    uint32_t lo = a0 > t->fba ? a0 - t->fba : 0;
    uint32_t hi = (a0 + bytes) - t->fba;
    if (hi > pitch * FB_H) hi = pitch * FB_H;
    uint32_t y0 = lo / pitch, y1 = (hi + pitch - 1) / pitch;
    if (y1 > FB_H) y1 = FB_H;
    uint32_t first = FB_H, last = 0;
    for (uint32_t y = y0; y < y1; y++) {
        uint32_t rs = y * pitch;
        if (lo < rs + draw && rs < hi) { if (y < first) first = y; last = y + 1; }
    }
    if (first >= last) return 0;
    *out_y0 = first; *out_y1 = last;
    return 1;
}

static int target_readback_rows(Target *t, uint32_t y0, uint32_t y1) {
    if (!t->gpu_valid) return 1;
    if (y1 > FB_H) y1 = FB_H;
    if (y0 >= y1) return 1;
    int whole = (y0 == 0 && y1 == FB_H);
    if (t == s_cur && s_nbatch) submit_pending();
    if (t->clean_gen == t->render_gen) return 1;
    if (!whole && t->part_gen == t->render_gen
        && y0 >= t->part_y0 && y1 <= t->part_y1) return 1;
    uint32_t rows = y1 - y0;
    XferSlot *x = xfer_acquire();
    if (!cmd_begin()) return 0;
    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageOffset.y = (int32_t)y0;
    c.imageExtent.width = FB_W; c.imageExtent.height = rows; c.imageExtent.depth = 1;
    VkImage srcimg = t->img;
    if (t->scale > 1) {
        VkImageBlit b = {0};
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.layerCount = 1;
        b.srcOffsets[0].y = (int32_t)(y0 * t->scale);
        b.srcOffsets[1].x = (int32_t)(FB_W * t->scale);
        b.srcOffsets[1].y = (int32_t)(y1 * t->scale);
        b.srcOffsets[1].z = 1;
        b.dstSubresource = b.srcSubresource;
        b.dstOffsets[0].y = (int32_t)y0;
        b.dstOffsets[1].x = FB_W; b.dstOffsets[1].y = (int32_t)y1; b.dstOffsets[1].z = 1;
        to_layout(s_cmd, t->rimg, VK_IMAGE_ASPECT_COLOR_BIT, &t->rlayout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdBlitImage(s_cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       t->rimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                       VK_FILTER_LINEAR);
        to_layout(s_cmd, t->rimg, VK_IMAGE_ASPECT_COLOR_BIT, &t->rlayout,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        srcimg = t->rimg;
    }
    vkCmdCopyImageToBuffer(s_cmd, srcimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, x->buf, 1, &c);
    if (!cmd_submit()) return 0;
    xfer_used(x);
    gpu_wait_id(x->id);
    u64 rback_t0 = g_prof_on ? ge_prof_now_tk() : 0;
    const uint32_t *src = (const uint32_t *)x->map;
    uint32_t wb = t->stride < DRAW_W ? t->stride : DRAW_W;
    for (uint32_t y = y0; y < y1; y++) {
        const uint32_t *row = src + (y - y0) * FB_W;
        if (t->fmt == 3) {
            memcpy((uint32_t *)SR_HOST((0x04000000u | t->fba) + y * t->stride * 4),
                   row, wb * 4);
            continue;
        }
        uint16_t *dst = (uint16_t *)SR_HOST((0x04000000u | t->fba) + y * t->stride * 2);
        switch (t->fmt & 3) {
        case 0:
            for (uint32_t x = 0; x < wb; x++) {
                uint32_t p = row[x];
                dst[x] = (uint16_t)(((p >> 3) & 0x1Fu)
                                  | (((p >> 10) & 0x3Fu) << 5)
                                  | (((p >> 19) & 0x1Fu) << 11));
            }
            break;
        case 1:
            for (uint32_t x = 0; x < wb; x++) {
                uint32_t p = row[x];
                dst[x] = (uint16_t)(((p >> 3) & 0x1Fu)
                                  | (((p >> 11) & 0x1Fu) << 5)
                                  | (((p >> 19) & 0x1Fu) << 10)
                                  | ((p & 0x80000000u) ? 0x8000u : 0u));
            }
            break;
        default:
            for (uint32_t x = 0; x < wb; x++) {
                uint32_t p = row[x];
                dst[x] = (uint16_t)(((p >> 4) & 0x0Fu)
                                  | (((p >> 12) & 0x0Fu) << 4)
                                  | (((p >> 20) & 0x0Fu) << 8)
                                  | (((p >> 28) & 0x0Fu) << 12));
            }
            break;
        }
    }
    if (g_prof_on) g_prof_rback_tk += ge_prof_now_tk() - rback_t0;
    if (whole) {
        t->clean_gen = t->render_gen;
        t->drawn_y0 = t->drawn_y1 = 0;
    } else { t->part_gen = t->render_gen; t->part_y0 = y0; t->part_y1 = y1; }
    s_cnt_readback++;
    s_cnt_rb_by_reason[s_rb_reason]++;
    s_cnt_rb_rows_by_reason[s_rb_reason] += rows;
    if (!whole) s_cnt_readback_rows++;
    return 1;
}

static int target_readback(Target *t) { return target_readback_rows(t, 0, FB_H); }

static int target_readback_range(Target *t, uint32_t a0, uint32_t bytes) {
    uint32_t y0, y1;
    if (!t->gpu_valid) return 1;
    static int fullrb = -1;
    if (fullrb < 0) fullrb = getenv("SR_GPU_FULLRB") ? 1 : 0;
    if (fullrb) return target_readback(t);
    if (!target_rows_for_range(t, a0, bytes, &y0, &y1)) { s_cnt_readback_skip++; return 1; }
    if (y0 < t->drawn_y0) y0 = t->drawn_y0;
    if (y1 > t->drawn_y1) y1 = t->drawn_y1;
    if (y0 >= y1) { s_cnt_readback_skip++; return 1; }
    return target_readback_rows(t, y0, y1);
}

static int target_upload(Target *t) {
    uint32_t *dst = s_pxscratch;
    for (uint32_t y = 0; y < FB_H; y++) {
        if (t->fmt == 3) {
            memcpy(dst + y * FB_W, (const uint32_t *)SR_HOST((0x04000000u | t->fba) + y * t->stride * 4),
                   t->stride * 4);
        } else {
            const uint16_t *src = (const uint16_t *)SR_HOST((0x04000000u | t->fba) + y * t->stride * 2);
            for (uint32_t x = 0; x < t->stride; x++) dst[y * FB_W + x] = fb_unpack(src[x], t->fmt);
        }
        for (uint32_t x = t->stride; x < FB_W; x++) dst[y * FB_W + x] = 0;
    }
    gpu_sync();
    XferSlot *x = xfer_acquire();
    memcpy(x->map, s_pxscratch, FB_W * FB_H * 4);
    if (!cmd_begin()) return 0;
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = FB_W; c.imageExtent.height = FB_H; c.imageExtent.depth = 1;
    if (t->scale > 1) {
        VkImageBlit b = {0};
        to_layout(s_cmd, t->rimg, VK_IMAGE_ASPECT_COLOR_BIT, &t->rlayout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(s_cmd, x->buf, t->rimg,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        to_layout(s_cmd, t->rimg, VK_IMAGE_ASPECT_COLOR_BIT, &t->rlayout,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.layerCount = 1;
        b.srcOffsets[1].x = FB_W; b.srcOffsets[1].y = FB_H; b.srcOffsets[1].z = 1;
        b.dstSubresource = b.srcSubresource;
        b.dstOffsets[1].x = (int32_t)(FB_W * t->scale);
        b.dstOffsets[1].y = (int32_t)(FB_H * t->scale);
        b.dstOffsets[1].z = 1;
        to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdBlitImage(s_cmd, t->rimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                       VK_FILTER_NEAREST);
    } else {
        to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(s_cmd, x->buf, t->img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!cmd_submit()) return 0;
    xfer_used(x);
    t->gpu_valid = 1;
    t->render_gen++;
    t->clean_gen = t->render_gen;
    t->drawn_y0 = t->drawn_y1 = 0;
    s_cnt_upload++;
    return 1;
}

static void target_destroy(Target *t) {
    if (!t->used) return;
    gpu_sync();
    if (s_snap_src == t) s_snap_src = NULL;
    if (t->fb) vkDestroyFramebuffer(s_dev, t->fb, NULL);
    if (t->set_n) vkFreeDescriptorSets(s_dev, s_dpool_fix, 1, &t->set_n);
    if (t->set_l) vkFreeDescriptorSets(s_dev, s_dpool_fix, 1, &t->set_l);
    if (t->view) vkDestroyImageView(s_dev, t->view, NULL);
    if (t->img) vkDestroyImage(s_dev, t->img, NULL);
    if (t->mem) vkFreeMemory(s_dev, t->mem, NULL);
    if (t->rimg) vkDestroyImage(s_dev, t->rimg, NULL);
    if (t->rmem) vkFreeMemory(s_dev, t->rmem, NULL);
    memset(t, 0, sizeof(*t));
}

static DepthEnt *depth_acquire(uint32_t zba, uint32_t zstride) {
    for (int i = 0; i < MAX_DEP; i++)
        if (s_deps[i].used && s_deps[i].zba == zba && s_deps[i].zstride == zstride
            && s_deps[i].scale == gpu_scale())
            return &s_deps[i];
    DepthEnt *d = NULL;
    for (int i = 0; i < MAX_DEP; i++) if (!s_deps[i].used) { d = &s_deps[i]; break; }
    if (!d) {
        gpu_sync(); vkDeviceWaitIdle(s_dev);
        d = &s_deps[0];
        vkDestroyImageView(s_dev, d->view, NULL);
        vkDestroyImage(s_dev, d->img, NULL);
        vkFreeMemory(s_dev, d->mem, NULL);
        for (int i = 0; i < MAX_TGT; i++)
            if (s_tgts[i].used && s_tgts[i].dep == d) {
                vkDestroyFramebuffer(s_dev, s_tgts[i].fb, NULL);
                s_tgts[i].fb = VK_NULL_HANDLE; s_tgts[i].dep = NULL;
            }
        memset(d, 0, sizeof(*d));
    }
    d->scale = gpu_scale();
    if (!make_image(FB_W * d->scale, FB_H * d->scale, VK_FORMAT_D16_UNORM,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    &d->img, &d->mem)) return NULL;
    if (!make_view(d->img, VK_FORMAT_D16_UNORM, VK_IMAGE_ASPECT_DEPTH_BIT, &d->view)) return NULL;
    d->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    d->zba = zba; d->zstride = zstride; d->used = 1;
    if (d->scale == 1) {
        XferSlot *x = xfer_acquire();
        memcpy(x->map, s_zbuf, FB_W * FB_H * 2);
        if (!cmd_begin()) return d;
        to_layout(s_cmd, d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c = {0};
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = FB_W; c.imageExtent.height = FB_H; c.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(s_cmd, x->buf, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        to_layout(s_cmd, d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        cmd_submit();
        xfer_used(x);
    }
    return d;
}

static void targets_rescale(void) {
    if (s_nbatch) submit_pending();
    gpu_sync(); vkDeviceWaitIdle(s_dev);
    for (int i = 0; i < MAX_TGT; i++)
        if (s_tgts[i].used) { target_readback(&s_tgts[i]); target_destroy(&s_tgts[i]); }
    for (int i = 0; i < MAX_DEP; i++) {
        DepthEnt *d = &s_deps[i];
        if (!d->used) continue;
        if (d->view) vkDestroyImageView(s_dev, d->view, NULL);
        if (d->img) vkDestroyImage(s_dev, d->img, NULL);
        if (d->mem) vkFreeMemory(s_dev, d->mem, NULL);
        memset(d, 0, sizeof(*d));
    }
    s_cur = NULL;
    s_snap_src = NULL;
}

static Target *target_acquire(void) {
    uint32_t fba = s_ge->fbp & 0x001FFFFFu;
    uint32_t stride = s_ge->fbw ? s_ge->fbw : 512;
    if (stride > 512) stride = 512;
    uint32_t fmt = s_ge->fbfmt & 3;
    uint32_t zba = s_ge->zbp & 0x001FFFFFu;
    uint32_t zstride = s_ge->zbw ? s_ge->zbw : stride;
    if (zstride > 512) zstride = 512;

    for (int i = 0; i < MAX_TGT; i++)
        if (s_tgts[i].used && s_tgts[i].scale != gpu_scale()) { targets_rescale(); break; }

    Target *t = NULL;
    for (int i = 0; i < MAX_TGT; i++)
        if (s_tgts[i].used && s_tgts[i].fba == fba) { t = &s_tgts[i]; break; }

    if (t && (t->stride != stride || t->fmt != fmt)) {
        s_rb_reason = RB_REACQUIRE;
        target_readback(t);
        s_rb_reason = RB_CLUT;
        if (t == s_cur) { submit_pending(); s_cur = NULL; }
        target_destroy(t);
        t = NULL;
    }

    if (!t) {
        for (int i = 0; i < MAX_TGT; i++) if (!s_tgts[i].used) { t = &s_tgts[i]; break; }
        if (!t) {
            Target *old = NULL;
            for (int i = 0; i < MAX_TGT; i++)
                if (s_tgts[i].used && (!old || s_tgts[i].lru < old->lru) && &s_tgts[i] != s_cur)
                    old = &s_tgts[i];
            s_rb_reason = RB_REACQUIRE;
            target_readback(old);
            s_rb_reason = RB_CLUT;
            if (old == s_cur) { submit_pending(); s_cur = NULL; }
            target_destroy(old);
            t = old;
        }
        memset(t, 0, sizeof(*t));
        t->fba = fba; t->stride = stride; t->fmt = fmt; t->used = 1;
        t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        t->rlayout = VK_IMAGE_LAYOUT_UNDEFINED;
        t->scale = gpu_scale();
        if (!make_image(FB_W * t->scale, FB_H * t->scale, VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        &t->img, &t->mem)) { t->used = 0; return NULL; }
        if (t->scale > 1
            && !make_image(FB_W, FB_H, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           &t->rimg, &t->rmem)) { t->used = 0; return NULL; }
        if (!make_view(t->img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, &t->view)) return NULL;
        t->set_n = make_descriptor(t->view, s_smp_n, s_dpool_fix);
        t->set_l = make_descriptor(t->view, s_smp_l, s_dpool_fix);
        if (!target_upload(t)) return NULL;
    } else if (!t->gpu_valid) {
        if (!target_upload(t)) return NULL;
    }

    DepthEnt *d = depth_acquire(zba, zstride);
    if (!d) return NULL;
    if (t->dep != d || !t->fb) {
        if (t->fb) { gpu_sync(); vkDestroyFramebuffer(s_dev, t->fb, NULL); }
        VkImageView views[2] = { t->view, d->view };
        VkFramebufferCreateInfo fbc = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbc.renderPass = s_rp; fbc.attachmentCount = 2; fbc.pAttachments = views;
        fbc.width = FB_W * t->scale; fbc.height = FB_H * t->scale; fbc.layers = 1;
        if (vkCreateFramebuffer(s_dev, &fbc, NULL, &t->fb) != VK_SUCCESS) return NULL;
        t->dep = d;
    }
    t->lru = s_lru++;
    return t;
}

static int begin_target(void) {
    uint32_t fba = s_ge->fbp & 0x001FFFFFu;
    uint32_t stride = s_ge->fbw ? s_ge->fbw : 512;
    if (stride > 512) stride = 512;
    if (s_cur && s_cur->used && s_cur->fba == fba &&
        s_cur->fmt == (s_ge->fbfmt & 3) && s_cur->stride == stride && s_cur->gpu_valid) {
        s_cur->lru = s_lru++;
        return 1;
    }
    if (s_nbatch) submit_pending();
    s_cur = target_acquire();
    return s_cur != NULL;
}

static uint64_t fnv64(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static uint64_t tex_hash(void) {
    uint32_t bpp_num;
    switch (s_ge->tex_fmt) {
        case 3: bpp_num = 32; break;
        case 4: bpp_num = 4;  break;
        case 5: bpp_num = 8;  break;
        default: bpp_num = 16; break;
    }
    uint32_t stride = s_ge->tex_bufw ? s_ge->tex_bufw : (uint32_t)s_ge->tex_w;
    uint64_t bytes = ((uint64_t)stride * (uint64_t)s_ge->tex_h * bpp_num) >> 3;
    if (bytes > 4u << 20) bytes = 4u << 20;
    const uint8_t *p = (const uint8_t *)SR_HOST(s_ge->tex_addr);
    uint64_t h = 1469598103934665603ull;
    h = fnv64(h, &bytes, sizeof(bytes));
    uint64_t step = bytes / 64; if (step < 16) step = 16;
    for (uint64_t off = 0; off + 16 <= bytes; off += step) h = fnv64(h, p + off, 16);
    if (bytes >= 16) h = fnv64(h, p + bytes - 16, 16);
    return h;
}

static uint64_t clut_hash(void) {
    static uint64_t cached;
    static uint32_t cached_gen = 0xFFFFFFFFu, cached_fmt = 0xFFFFFFFFu;
    if (cached_gen == s_ge->clut_gen && cached_fmt == s_ge->clut_fmt) return cached;
    uint64_t h = 1469598103934665603ull;
    h = fnv64(h, s_ge->clutram, sizeof(s_ge->clutram));
    h = fnv64(h, &s_ge->clut_fmt, sizeof(s_ge->clut_fmt));
    cached = h;
    cached_gen = s_ge->clut_gen;
    cached_fmt = s_ge->clut_fmt;
    return h;
}

static uint64_t s_texlru = 1;
static uint64_t s_tex_budget = 0;

static uint64_t tex_budget(void) {
    if (!s_tex_budget) {
        const char *e = getenv("PSPRECO_TEX_BUDGET_MB");
        unsigned long mb = e && e[0] ? strtoul(e, NULL, 10) : 0;
        if (!mb) mb = 1024;
        s_tex_budget = (uint64_t)mb << 20;
    }
    return s_tex_budget;
}

static void tex_free_entry(TexEnt *e) {
    if (e->set)  vkFreeDescriptorSets(s_dev, s_dpool_tex, 1, &e->set);
    if (e->smp)  vkDestroySampler(s_dev, e->smp, NULL);
    if (e->view) vkDestroyImageView(s_dev, e->view, NULL);
    if (e->img)  vkDestroyImage(s_dev, e->img, NULL);
    if (e->mem)  vkFreeMemory(s_dev, e->mem, NULL);
    s_tex_bytes -= e->bytes;
    e->set = VK_NULL_HANDLE; e->smp = VK_NULL_HANDLE;
    e->view = VK_NULL_HANDLE; e->img = VK_NULL_HANDLE; e->mem = VK_NULL_HANDLE;
    e->bytes = 0;
}

static void tex_evict_to(int goal, uint64_t byte_goal) {
    if (s_tex_n <= goal && s_tex_bytes <= byte_goal) return;
    gpu_sync(); vkDeviceWaitIdle(s_dev);
    while (s_tex_n > 0 && (s_tex_n > goal || s_tex_bytes > byte_goal)) {
        int v = 0;
        for (int i = 1; i < s_tex_n; i++)
            if (s_tex[i].lru < s_tex[v].lru) v = i;
        tex_free_entry(&s_tex[v]);
        s_tex[v] = s_tex[--s_tex_n];
    }
}

static void tex_evict_lru(void) {
    int goal = s_tex_n;
    uint64_t budget = tex_budget();
    if (s_tex_n >= MAX_TEX) goal = s_tex_n - MAX_TEX / 4;
    tex_evict_to(goal, budget - budget / 4);
}

static void mip_barrier(VkCommandBuffer cmd, VkImage img, uint32_t level,
                        VkImageLayout from, VkImageLayout to,
                        VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    mb.srcAccessMask = src; mb.dstAccessMask = dst;
    mb.oldLayout = from; mb.newLayout = to;
    mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = img;
    mb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    mb.subresourceRange.baseMipLevel = level;
    mb.subresourceRange.levelCount = 1;
    mb.subresourceRange.layerCount = 1;
    VkPipelineStageFlags dst_stage = (dst & VK_ACCESS_SHADER_READ_BIT)
                                   ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                   : VK_PIPELINE_STAGE_TRANSFER_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         dst_stage, 0, 0, NULL, 0, NULL, 1, &mb);
}

static VkBuffer       s_texstage;
static VkDeviceMemory s_texstage_mem;
static void          *s_texstage_map;
static VkDeviceSize   s_texstage_size;
static uint64_t       s_texstage_id;

static int texstage_reserve(VkDeviceSize need) {
    if (s_texstage && s_texstage_size >= need) {
        gpu_wait_id(s_texstage_id);
        return 1;
    }
    gpu_sync();
    if (s_texstage) {
        vkDestroyBuffer(s_dev, s_texstage, NULL);
        vkFreeMemory(s_dev, s_texstage_mem, NULL);
        s_texstage = VK_NULL_HANDLE;
        s_texstage_mem = VK_NULL_HANDLE;
        s_texstage_map = NULL;
        s_texstage_size = 0;
    }
    VkDeviceSize want = XFER_SIZE;
    while (want < need) want *= 2;
    if (!make_buffer(want, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     &s_texstage, &s_texstage_mem, &s_texstage_map))
        return 0;
    s_texstage_size = want;
    return 1;
}

static int tex_upload(VkImage img, const TexLevel *lv, int nlv, uint32_t levels) {
    VkDeviceSize total = 0, off = 0;
    for (int i = 0; i < nlv; i++)
        total += (VkDeviceSize)lv[i].w * (VkDeviceSize)lv[i].h * 4;
    if (!total || !texstage_reserve(total)) return 0;
    for (int i = 0; i < nlv; i++) {
        size_t n = (size_t)lv[i].w * (size_t)lv[i].h * 4;
        memcpy((uint8_t *)s_texstage_map + off, lv[i].px, n);
        off += n;
    }
    if (!cmd_begin()) return 0;

    off = 0;
    for (int i = 0; i < nlv; i++) {
        mip_barrier(s_cmd, img, (uint32_t)i, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy bic = {0};
        bic.bufferOffset = off;
        bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bic.imageSubresource.mipLevel = (uint32_t)i;
        bic.imageSubresource.layerCount = 1;
        bic.imageExtent.width = (uint32_t)lv[i].w;
        bic.imageExtent.height = (uint32_t)lv[i].h;
        bic.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(s_cmd, s_texstage, img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        off += (VkDeviceSize)lv[i].w * (VkDeviceSize)lv[i].h * 4;
    }

    int mw = lv[nlv - 1].w, mh = lv[nlv - 1].h;
    for (uint32_t l = (uint32_t)nlv; l < levels; l++) {
        int nw = mw > 1 ? mw / 2 : 1, nh = mh > 1 ? mh / 2 : 1;
        mip_barrier(s_cmd, img, l - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        mip_barrier(s_cmd, img, l, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageBlit b = {0};
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.mipLevel = l - 1;
        b.srcSubresource.layerCount = 1;
        b.srcOffsets[1].x = mw; b.srcOffsets[1].y = mh; b.srcOffsets[1].z = 1;
        b.dstSubresource = b.srcSubresource;
        b.dstSubresource.mipLevel = l;
        b.dstOffsets[1].x = nw; b.dstOffsets[1].y = nh; b.dstOffsets[1].z = 1;
        vkCmdBlitImage(s_cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        mip_barrier(s_cmd, img, l - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
        mw = nw; mh = nh;
    }

    mip_barrier(s_cmd, img, levels - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    for (uint32_t l = 0; l + 1 < (uint32_t)nlv; l++)
        mip_barrier(s_cmd, img, l, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    if (!cmd_submit()) return 0;
    s_texstage_id = s_submit_id;
    return 1;
}

static uint64_t tex_size_bytes(int w, int h, uint32_t levels) {
    uint64_t n = 0;
    int mw = w, mh = h;
    for (uint32_t l = 0; l < levels; l++) {
        n += (uint64_t)mw * (uint64_t)mh * 4;
        mw = mw > 1 ? mw / 2 : 1;
        mh = mh > 1 ? mh / 2 : 1;
    }
    return n;
}

static int tex_make(TexEnt *e, const TexLevel *lv, int nlv, uint32_t levels,
                    int linear, int clamp_u, int clamp_v) {
    int w = lv[0].w, h = lv[0].h;
    if (!make_image_mip((uint32_t)w, (uint32_t)h, levels, VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                        | VK_IMAGE_USAGE_SAMPLED_BIT, &e->img, &e->mem))
        return 0;
    e->bytes = tex_size_bytes(w, h, levels);
    s_tex_bytes += e->bytes;
    if (!tex_upload(e->img, lv, nlv, levels)) return 0;
    if (!make_view_mip(e->img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                       levels, &e->view)) return 0;
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.minFilter = sci.magFilter;
    sci.mipmapMode = levels > 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.maxLod = (float)(levels - 1);
    sci.addressModeU = clamp_u ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = clamp_v ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (levels > 1 && s_max_aniso > 1.0f && g_gfx.anisotropy > 1) {
        float want = (float)g_gfx.anisotropy;
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = want < s_max_aniso ? want : s_max_aniso;
    }
    VKC(vkCreateSampler(s_dev, &sci, NULL, &e->smp));
    e->set = make_descriptor(e->view, e->smp, s_dpool_tex);
    if (!e->set) return 0;
    e->w = w; e->h = h; e->levels = levels;
    e->linear = linear; e->clamp_u = clamp_u; e->clamp_v = clamp_v;
    return 1;
}

static void tex_cache_flush(void) {
    if (!s_tex_n) return;
    submit_pending();
    gpu_sync(); vkDeviceWaitIdle(s_dev);
    for (int i = 0; i < s_tex_n; i++) tex_free_entry(&s_tex[i]);
    s_tex_n = 0;
    s_tex_bytes = 0;
}

static float s_tex_half[2];
static uint32_t s_video_scaled[1024*1024], s_video_tmp[1024*512];

static int movie_texture(int *w, int *h) {
    if (!g_gfx.movie_clarity || s_ge->tex_swizzle) return 0;
    return mpeg_video_surface(s_ge->tex_addr, (int)s_ge->tex_fmt,
                              s_ge->tex_bufw & 0x7ffu, w, h);
}

static int tex_fill(TexEnt *e, int pw, int ph, int linear, int clamp_u, int clamp_v) {
    TexRepImage rep;
    TexRepId id;
    TexLevel lv[TEXREP_MAX_LEVELS];
    int nlv = 1, have = 0, ident = 0, filt = linear;
    uint32_t levels;
    int movie_w=0, movie_h=0;
    int movie=movie_texture(&movie_w,&movie_h) && movie_w<=pw && movie_h<=ph;

    memset(&rep, 0, sizeof rep);
    memset(&id, 0, sizeof id);

    if (!movie && (texrep_replace_on() || texrep_dump_on())) ident = texrep_identify(&id);
    if (ident) have = texrep_find(&id, &rep);

    if (have) {
        if (rep.filtering == TEXREP_FILTER_NEAREST) filt = 0;
        else if (rep.filtering == TEXREP_FILTER_LINEAR) filt = 1;
        nlv = rep.levels;
        for (int i = 0; i < nlv; i++) {
            lv[i].px = rep.px[i];
            lv[i].w = rep.w[i];
            lv[i].h = rep.h[i];
        }
    } else {
        ge_decode_tex_rgba(s_texscratch);
        if (ident) texrep_dump(&id, s_texscratch, pw, ph);
        lv[0].px = s_texscratch;
        lv[0].w = pw;
        lv[0].h = ph;
        if (movie) {
            video_upscale2x(s_texscratch,pw,movie_w,movie_h,s_video_scaled,pw*2,ph*2,s_video_tmp);
            lv[0].px=s_video_scaled; lv[0].w=pw*2; lv[0].h=ph*2; filt=1;
            static int logged;
            if (!logged && getenv("PSPRECO_TEX_LOG")) {
                logged=1;
                fprintf(stderr,"[video] cinematic cubic upscale %dx%d -> %dx%d (texture %dx%d)\n",
                        movie_w,movie_h,movie_w*2,movie_h*2,pw*2,ph*2);
            }
        }
    }

    if (!filt || !g_gfx.mipmaps_on) nlv = 1;
    levels = (uint32_t)nlv;
    if (nlv == 1 && filt && g_gfx.mipmaps_on) levels = mip_levels_for(lv[0].w, lv[0].h);
    if (movie) levels=1;

    if (e->img && (e->w != lv[0].w || e->h != lv[0].h || e->levels != levels ||
                   e->linear != filt || e->clamp_u != clamp_u || e->clamp_v != clamp_v)) {
        gpu_sync(); vkDeviceWaitIdle(s_dev);
        tex_free_entry(e);
    }

    int ok;
    if (!e->img) ok = tex_make(e, lv, nlv, levels, filt, clamp_u, clamp_v);
    else         ok = tex_upload(e->img, lv, nlv, levels);

    e->half_u = have ? 0.5f * (float)pw / (float)lv[0].w - 0.5f : 0.0f;
    e->half_v = have ? 0.5f * (float)ph / (float)lv[0].h - 0.5f : 0.0f;

    if (have) { texrep_release(&rep); if (ok) s_cnt_repl++; }
    return ok;
}

static VkDescriptorSet tex_bind(TexEnt *e) {
    s_tex_half[0] = e->half_u;
    s_tex_half[1] = e->half_v;
    return e->set;
}

static VkDescriptorSet tex_get_inner(void) {
    int w = s_ge->tex_w, h = s_ge->tex_h;
    static unsigned tex_gen;
    s_tex_half[0] = s_tex_half[1] = 0.0f;
    if (tex_gen != g_gfx_generation) { tex_gen = g_gfx_generation; tex_cache_flush(); }
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return s_white_set;
    int linear  = (s_ge->tex_filter & 1) || ((s_ge->tex_filter >> 8) & 1);
    int movie_w, movie_h;
    if (movie_texture(&movie_w,&movie_h)) linear=1;
    int clamp_u = s_ge->tex_wrap & 1, clamp_v = (s_ge->tex_wrap >> 8) & 1;
    if (g_gfx.tex_filter == TEXF_LINEAR) clamp_u = clamp_v = 1;
    uint64_t key = (uint64_t)s_ge->tex_addr | ((uint64_t)s_ge->tex_fmt << 32)
                 | ((uint64_t)(uint32_t)w << 36) | ((uint64_t)(uint32_t)h << 46)
                 | ((uint64_t)s_ge->tex_swizzle << 56) | ((uint64_t)(unsigned)linear << 57)
                 | ((uint64_t)(unsigned)clamp_u << 58) | ((uint64_t)(unsigned)clamp_v << 59);
    key ^= (uint64_t)s_ge->tex_bufw << 16;
    if (s_ge->tex_fmt == 4 || s_ge->tex_fmt == 5)
        key ^= clut_hash() | 1;
    uint64_t hash = tex_hash();
    for (int i = 0; i < s_tex_n; i++) {
        TexEnt *e = &s_tex[i];
        if (e->key != key) continue;
        e->lru = s_texlru++;
        if (e->hash == hash && e->set) { e->pending = 1; return tex_bind(e); }
        if (e->pending) submit_pending();
        gpu_sync();
        if (!tex_fill(e, w, h, linear, clamp_u, clamp_v)) {
            tex_free_entry(e);
            *e = s_tex[--s_tex_n];
            s_tex_half[0] = s_tex_half[1] = 0.0f;
            return s_white_set;
        }
        e->hash = hash;
        e->pending = 1;
        s_cnt_texup++;
        return tex_bind(e);
    }

    if (s_tex_n >= MAX_TEX || s_tex_bytes > tex_budget()) {
        submit_pending();
        tex_evict_lru();
    }
    TexEnt *e = &s_tex[s_tex_n];
    memset(e, 0, sizeof(*e));
    if (!tex_fill(e, w, h, linear, clamp_u, clamp_v)) {
        tex_free_entry(e);
        s_tex_half[0] = s_tex_half[1] = 0.0f;
        return s_white_set;
    }
    e->key = key; e->hash = hash;
    e->pending = 1;
    e->lru = s_texlru++;
    s_cnt_texup++;
    s_tex_n++;
    return tex_bind(e);
}

static VkDescriptorSet tex_get(void) {
    if (!g_prof_on) return tex_get_inner();
    u64 t0 = ge_prof_now_tk();
    VkDescriptorSet set = tex_get_inner();
    g_prof_tex_tk += ge_prof_now_tk() - t0;
    return set;
}

static float snap16(float v) { return floorf(v * 16.0f + 0.375f) * (1.0f / 16.0f); }

#define NO_UV_BOUND 1.0e30f

static float s_ulo = -NO_UV_BOUND, s_vlo = -NO_UV_BOUND;
static float s_uhi =  NO_UV_BOUND, s_vhi =  NO_UV_BOUND;

static void uv_bound_set(float u0, float u1, float v0, float v1) {
    s_ulo = u0 < u1 ? u0 : u1;  s_uhi = u0 < u1 ? u1 : u0;
    s_vlo = v0 < v1 ? v0 : v1;  s_vhi = v0 < v1 ? v1 : v0;
}
static void uv_bound_clear(void) {
    s_ulo = s_vlo = -NO_UV_BOUND;
    s_uhi = s_vhi =  NO_UV_BOUND;
}

static void put_vert(float x, float y, float z, float rw, float u, float v, float fog,
                     int r, int g, int b, int a) {
    GpuVert *o = &s_vstage[s_nverts++];
    o->x = snap16(x); o->y = snap16(y); o->z = z; o->rw = rw;
    o->u = u; o->v = v; o->fog = fog;
    o->ulo = s_ulo; o->vlo = s_vlo; o->uhi = s_uhi; o->vhi = s_vhi;
    uint32_t cr = (uint32_t)(r < 0 ? 0 : r > 255 ? 255 : r);
    uint32_t cg = (uint32_t)(g < 0 ? 0 : g > 255 ? 255 : g);
    uint32_t cb = (uint32_t)(b < 0 ? 0 : b > 255 ? 255 : b);
    uint32_t ca = (uint32_t)(a < 0 ? 0 : a > 255 ? 255 : a);
    o->rgba = cr | (cg << 8) | (cb << 16) | (ca << 24);
}

static int map_factor(uint32_t f, int src_side, uint32_t fixed, int *need_const, int *premul) {
    switch (f & 0xF) {
        case 0: return src_side ? VK_BLEND_FACTOR_DST_COLOR : VK_BLEND_FACTOR_SRC_COLOR;
        case 1: return src_side ? VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 2: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 3: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 4: return VK_BLEND_FACTOR_DST_ALPHA;
        case 5: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 6:
            if (src_side) { *premul = F_SA2X; return VK_BLEND_FACTOR_ONE; }
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case 7:
            if (src_side) { *premul = F_SA2XI; return VK_BLEND_FACTOR_ONE; }
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 8: return VK_BLEND_FACTOR_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        default:
            if ((fixed & 0xFFFFFF) == 0)        return VK_BLEND_FACTOR_ZERO;
            if ((fixed & 0xFFFFFF) == 0xFFFFFF) return VK_BLEND_FACTOR_ONE;
            *need_const = 1;
            return VK_BLEND_FACTOR_CONSTANT_COLOR;
    }
}

static void build_state(int persp, int sprite, Batch *b) {
    GeState *g = s_ge;
    PipeKey *k = &b->key;
    memset(b, 0, sizeof(*b));

    int clear = g->clear;
    int premul = 0;

    int sx1 = g->scis_x1 > 0 ? g->scis_x1 : 0, sy1 = g->scis_y1 > 0 ? g->scis_y1 : 0;
    int sx2 = g->scis_x2 < DRAW_W - 1 ? g->scis_x2 : DRAW_W - 1;
    int sy2 = g->scis_y2 < FB_H - 1 ? g->scis_y2 : FB_H - 1;
    if (sx1 > sx2 || sy1 > sy2) { b->sw = 0; return; }
    b->sx = sx1; b->sy = sy1; b->sw = sx2 - sx1 + 1; b->sh = sy2 - sy1 + 1;

    if (clear) {
        k->cmask = ((g->clear_mode & 0x100) ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                               VK_COLOR_COMPONENT_B_BIT) : 0)
                 | ((g->clear_mode & 0x200) ? VK_COLOR_COMPONENT_A_BIT : 0);
    } else {
        uint32_t mr = g->mask_rgb & 0xFF, mg = (g->mask_rgb >> 8) & 0xFF, mb = (g->mask_rgb >> 16) & 0xFF;
        uint32_t ma = (uint32_t)g->mask_alpha & 0xFF;
        k->cmask = (mr >= 0x80 ? 0 : VK_COLOR_COMPONENT_R_BIT) | (mg >= 0x80 ? 0 : VK_COLOR_COMPONENT_G_BIT)
                 | (mb >= 0x80 ? 0 : VK_COLOR_COMPONENT_B_BIT) | (ma >= 0x80 ? 0 : VK_COLOR_COMPONENT_A_BIT);
    }

    b->bconst[3] = 1.0f;
    if (!clear && g->blend_enable) {
        uint32_t eq = (g->blend_mode >> 8) & 7;
        int nc_a = 0, nc_b = 0;
        int sf = map_factor(g->blend_mode & 0xF, 1, g->blend_fixa, &nc_a, &premul);
        int df = map_factor((g->blend_mode >> 4) & 0xF, 0, g->blend_fixb, &nc_b, &premul);
        if (nc_a && nc_b && (g->blend_fixa & 0xFFFFFF) != (g->blend_fixb & 0xFFFFFF)) {
            uint32_t fb_ = g->blend_fixb;
            uint32_t lum = (fb_ & 0xFF) + ((fb_ >> 8) & 0xFF) + ((fb_ >> 16) & 0xFF);
            df = lum >= 384 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
            nc_b = 0;
        }
        uint32_t fixed = nc_a ? g->blend_fixa : g->blend_fixb;
        b->bconst[0] = (float)(fixed & 0xFF) / 255.0f;
        b->bconst[1] = (float)((fixed >> 8) & 0xFF) / 255.0f;
        b->bconst[2] = (float)((fixed >> 16) & 0xFF) / 255.0f;
        k->blend_on = 1;
        k->srcf = (uint8_t)sf; k->dstf = (uint8_t)df;
        switch (eq) {
            case 1:  k->eq = VK_BLEND_OP_SUBTRACT; break;
            case 2:  k->eq = VK_BLEND_OP_REVERSE_SUBTRACT; break;
            case 3:  k->eq = VK_BLEND_OP_MIN; break;
            case 4:  k->eq = VK_BLEND_OP_MAX; break;
            case 5:  k->eq = VK_BLEND_OP_SUBTRACT; break;
            default: k->eq = VK_BLEND_OP_ADD; break;
        }
    }

    static const uint8_t zmap[8] = {
        VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_NOT_EQUAL,
        VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL,
    };
    if (clear) {
        k->ztest = k->zwrite = (g->clear_mode & 0x400) ? 1 : 0;
        k->zfunc = VK_COMPARE_OP_ALWAYS;
    } else {
        k->ztest = g->ztest_enable ? 1 : 0;
        k->zwrite = (k->ztest && !g->zwrite_disable) ? 1 : 0;
        k->zfunc = k->ztest ? zmap[g->ztest & 7] : VK_COMPARE_OP_ALWAYS;
    }

    k->cull = (!sprite && g->cull_enable && !clear) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;

    int textured = g->tex_enable && !clear && g->tex_addr;
    int flags = premul;
    if (clear) flags |= F_CLEAR;
    if (persp && !(clear && sprite)) flags |= F_PERSP;
    if (persp && g->fog_enable && !clear) flags |= F_FOG;
    b->pc.texsize[0] = b->pc.texsize[1] = 1.0f;
    if (textured) {
        flags |= F_TEX;
        if (g->tex_func & 0x100)   flags |= F_RGBA;
        if (g->tex_func & 0x10000) flags |= F_DBL;
        int linear = (g->tex_filter & 1) || ((g->tex_filter >> 8) & 1);
        if (!linear) flags |= F_NEAREST;
        b->pc.texsize[0] = (float)g->tex_w;
        b->pc.texsize[1] = (float)g->tex_h;
    }
    b->pc.cfg[1] = (!clear && g->atest_enable) ? (int32_t)g->atest : 1;
    b->pc.cfg[2] = (int32_t)g->minz;
    b->pc.cfg[3] = (int32_t)g->maxz;
    b->pc.texenv[0] = (float)(g->tex_env & 0xFF) / 255.0f;
    b->pc.texenv[1] = (float)((g->tex_env >> 8) & 0xFF) / 255.0f;
    b->pc.texenv[2] = (float)((g->tex_env >> 16) & 0xFF) / 255.0f;
    b->pc.fogcol[0] = (float)(g->fog_color & 0xFF) / 255.0f;
    b->pc.fogcol[1] = (float)((g->fog_color >> 8) & 0xFF) / 255.0f;
    b->pc.fogcol[2] = (float)((g->fog_color >> 16) & 0xFF) / 255.0f;

    b->dset = s_white_set;
    if (textured) {
        Target *src = NULL;
        if ((g->tex_addr & 0x0F000000u) == 0x04000000u) {
            uint32_t ta = g->tex_addr & 0x001FFFFFu;
            for (int i = 0; i < MAX_TGT; i++) {
                Target *ti = &s_tgts[i];
                if (!ti->used || !ti->gpu_valid) continue;
                uint32_t bpp_t = ti->fmt == 3 ? 4 : 2;
                uint32_t flen = ti->stride * FB_H * bpp_t;
                if (ta >= ti->fba && ta < ti->fba + flen) { src = ti; break; }
            }
        }
        if (src) {
            static int nortt_env = -1;
            if (nortt_env < 0) { const char *nv = getenv("SR_GPU_NORTT");
                                 nortt_env = (nv && nv[0] && strcmp(nv, "0") != 0) ? 1 : 0; }
            int nortt = nortt_env || !g_gfx.rtt;
            int linear = (g->tex_filter & 1) || ((g->tex_filter >> 8) & 1);
            uint32_t bpp_s = src->fmt == 3 ? 4 : 2;
            uint32_t toff = (g->tex_addr & 0x001FFFFFu) - src->fba;
            s_cnt_rtt++;
            if (!nortt && g->tex_fmt == src->fmt && !g->tex_swizzle &&
                (g->tex_bufw == src->stride || g->tex_bufw == 0) && (toff % bpp_s) == 0) {
                uint32_t pix = toff / bpp_s;
                if (src == s_cur) {
                    if (s_nbatch) submit_pending();
                    if (s_snap_src != src || s_snap_srcgen != src->render_gen) {
                        if (cmd_begin()) {
                            to_layout(s_cmd, src->img, VK_IMAGE_ASPECT_COLOR_BIT, &src->layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                            to_layout(s_cmd, s_snapimg, VK_IMAGE_ASPECT_COLOR_BIT, &s_snap_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                            VkImageBlit ib = {0};
                            ib.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                            ib.srcSubresource.layerCount = 1;
                            ib.srcOffsets[1].x = (int32_t)(FB_W * src->scale);
                            ib.srcOffsets[1].y = (int32_t)(FB_H * src->scale);
                            ib.srcOffsets[1].z = 1;
                            ib.dstSubresource = ib.srcSubresource;
                            ib.dstOffsets[1].x = FB_W;
                            ib.dstOffsets[1].y = FB_H;
                            ib.dstOffsets[1].z = 1;
                            vkCmdBlitImage(s_cmd, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           s_snapimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ib,
                                           src->scale > 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
                            to_layout(s_cmd, s_snapimg, VK_IMAGE_ASPECT_COLOR_BIT, &s_snap_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                            to_layout(s_cmd, src->img, VK_IMAGE_ASPECT_COLOR_BIT, &src->layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                            cmd_submit();
                        }
                        s_snap_src = src;
                        s_snap_srcgen = src->render_gen;
                        s_cnt_snap++;
                    }
                    b->dset = linear ? s_snap_l : s_snap_n;
                } else {
                    if (s_nbatch && src->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                        submit_pending();
                    if (src->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && cmd_begin()) {
                        to_layout(s_cmd, src->img, VK_IMAGE_ASPECT_COLOR_BIT, &src->layout,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        cmd_submit();
                    }
                    b->dset = linear ? src->set_l : src->set_n;
                }
                b->pc.texsize[0] = (float)FB_W;
                b->pc.texsize[1] = (float)FB_H;
                b->pc.texsize[2] = (float)(pix % src->stride);
                b->pc.texsize[3] = (float)(pix / src->stride);
            } else {
                s_rb_reason = RB_RTT;
                target_readback(src);
                s_rb_reason = RB_CLUT;
                b->dset = tex_get();
                if (!sprite) {
                    b->pc.texsize[2] = s_tex_half[0];
                    b->pc.texsize[3] = s_tex_half[1];
                }
            }
        } else {
            b->dset = tex_get();
            if (!sprite) {
                b->pc.texsize[2] = s_tex_half[0];
                b->pc.texsize[3] = s_tex_half[1];
            }
        }
    }
    b->pc.cfg[0] = (int32_t)((g->tex_func & 7) | ((uint32_t)flags << 8));
}

typedef struct {
    uint32_t clear, clear_mode;
    int blend_enable; uint32_t blend_mode, fixa, fixb;
    uint32_t mask_rgb; int mask_alpha;
    int scis[4];
    int ztest_enable, zwrite_disable; uint32_t ztest, minz, maxz;
    int cull_enable; uint32_t cull;
    int tex_enable; uint32_t tex_addr, tex_bufw, tex_fmt, tex_wrap, tex_func, tex_env, tex_filter;
    int tex_w, tex_h; int tex_swizzle;
    uint32_t clut_fmt, clut_gen;
    int atest_enable; uint32_t atest;
    int fog_enable; uint32_t fog_color;
    int persp, sprite;
    uint32_t flushgen;
} StateSnap;
static StateSnap s_snapst;
static Batch     s_tmpl;
static int       s_tmpl_ok = 0;

static void snap_fill(StateSnap *o, int persp, int sprite) {
    GeState *g = s_ge;
    memset(o, 0, sizeof(*o));
    o->clear = (uint32_t)g->clear; o->clear_mode = g->clear_mode;
    o->blend_enable = g->blend_enable; o->blend_mode = g->blend_mode;
    o->fixa = g->blend_fixa; o->fixb = g->blend_fixb;
    o->mask_rgb = g->mask_rgb; o->mask_alpha = g->mask_alpha;
    o->scis[0] = g->scis_x1; o->scis[1] = g->scis_y1; o->scis[2] = g->scis_x2; o->scis[3] = g->scis_y2;
    o->ztest_enable = g->ztest_enable; o->zwrite_disable = g->zwrite_disable;
    o->ztest = g->ztest; o->minz = g->minz; o->maxz = g->maxz;
    o->cull_enable = g->cull_enable; o->cull = g->cull;
    o->tex_enable = g->tex_enable; o->tex_addr = g->tex_addr; o->tex_bufw = g->tex_bufw;
    o->tex_fmt = g->tex_fmt; o->tex_wrap = g->tex_wrap; o->tex_func = g->tex_func;
    o->tex_env = g->tex_env; o->tex_filter = g->tex_filter;
    o->tex_w = g->tex_w; o->tex_h = g->tex_h; o->tex_swizzle = g->tex_swizzle;
    o->clut_fmt = g->clut_fmt; o->clut_gen = g->clut_gen;
    o->atest_enable = g->atest_enable; o->atest = g->atest;
    o->fog_enable = g->fog_enable; o->fog_color = g->fog_color;
    o->persp = persp; o->sprite = sprite;
    o->flushgen = s_flushgen;
}

extern unsigned long g_ge_state_gen;
static unsigned long s_tmpl_gen;
static int s_tmpl_persp = -1, s_tmpl_sprite = -1;
static uint32_t s_tmpl_flushgen;

static void state_get(int persp, int sprite, Batch *out) {
    if (s_tmpl_ok && s_tmpl_gen == g_ge_state_gen && s_tmpl_persp == persp
        && s_tmpl_sprite == sprite && s_tmpl_flushgen == s_flushgen) {
        *out = s_tmpl;
        return;
    }
    {
        StateSnap sn; snap_fill(&sn, persp, sprite);
        if (s_tmpl_ok && !memcmp(&sn, &s_snapst, sizeof(sn))) {
            s_tmpl_gen = g_ge_state_gen; s_tmpl_persp = persp;
            s_tmpl_sprite = sprite; s_tmpl_flushgen = s_flushgen;
            *out = s_tmpl;
            return;
        }
    }
    begin_target();
    build_state(persp, sprite, &s_tmpl);
    snap_fill(&s_snapst, persp, sprite);
    s_tmpl_ok = 1;
    s_tmpl_gen = g_ge_state_gen; s_tmpl_persp = persp;
    s_tmpl_sprite = sprite; s_tmpl_flushgen = s_flushgen;
    *out = s_tmpl;
}

static void append(Batch *b, uint32_t first, uint32_t count) {
    if (b->sw <= 0 || count == 0) return;
    if (s_nbatch) {
        Batch *last = &s_batch[s_nbatch - 1];
        if (last->first + last->count == first &&
            !memcmp(&last->key, &b->key, sizeof(b->key)) &&
            last->sx == b->sx && last->sy == b->sy && last->sw == b->sw && last->sh == b->sh &&
            !memcmp(last->bconst, b->bconst, sizeof(b->bconst)) &&
            !memcmp(&last->pc, &b->pc, sizeof(b->pc)) && last->dset == b->dset) {
            last->count += count;
            return;
        }
    }
    b->first = first; b->count = count;
    s_batch[s_nbatch++] = *b;
}

static void ensure_room(uint32_t verts) {
    if (s_nverts + verts > MAX_VERTS || s_nbatch >= MAX_BATCH - 1)
        submit_pending();
}

static int hook_tri(const GeVtx *A, const GeVtx *B, const GeVtx *C, int persp) {
    if (!s_ready) return 0;
    static int nocull = -1;
    if (nocull < 0) nocull = getenv("SR_NOCULL") ? 1 : 0;
    Batch b;
    state_get(persp, 0, &b);
    if (nocull) b.key.cull = VK_CULL_MODE_NONE;
    if (!begin_target()) return 1;
    ensure_room(3);
    uint32_t first = s_nverts;
    int flat = !s_ge->shade_gouraud || s_ge->clear;
    const GeVtx *ca = flat ? C : A;
    const GeVtx *cb = flat ? C : B;
    put_vert(A->x, A->y, A->z, A->rw, A->u, A->v, A->fog, ca->r, ca->g, ca->b, ca->a);
    put_vert(B->x, B->y, B->z, B->rw, B->u, B->v, B->fog, cb->r, cb->g, cb->b, cb->a);
    put_vert(C->x, C->y, C->z, C->rw, C->u, C->v, C->fog, C->r, C->g, C->b, C->a);
    append(&b, first, 3);
    s_cnt_tri++;
    return 1;
}

static int hook_sprite(const GeVtx *p0, const GeVtx *p1, int persp) {
    if (!s_ready) return 0;
    Batch b;
    state_get(persp, 1, &b);
    if (!begin_target()) return 1;

    float xa = floorf(fminf(p0->x, p1->x)), xb = floorf(fmaxf(p0->x, p1->x));
    float ya = floorf(fminf(p0->y, p1->y)), yb = floorf(fmaxf(p0->y, p1->y));
    if (xb <= xa || yb <= ya) return 1;

    float u0 = p0->u, v0 = p0->v, u1 = p1->u, v1 = p1->v;
    if (persp) {
        u0 = p0->u / (p0->rw != 0.0f ? p0->rw : 1.0f);
        v0 = p0->v / (p0->rw != 0.0f ? p0->rw : 1.0f);
        u1 = p1->u / (p1->rw != 0.0f ? p1->rw : 1.0f);
        v1 = p1->v / (p1->rw != 0.0f ? p1->rw : 1.0f);
    }
    int flipx = p0->x > p1->x, flipy = p0->y > p1->y;
    if (flipx) { float t = u0; u0 = u1; u1 = t; }
    if (flipy) { float t = v0; v0 = v1; v1 = t; }
    float uw = (u1 - u0) / (xb - xa), vw = (v1 - v0) / (yb - ya);
    if (flipx) { u0 += uw; u1 += uw; }
    if (flipy) { v0 += vw; v1 += vw; }
    float ua = u0 - 0.5f * uw, ub = u1 - 0.5f * uw;
    float va = v0 - 0.5f * vw, vb = v1 - 0.5f * vw;

    float z = p1->z, fog = p1->fog;
    int r = p1->r, g = p1->g, bb_ = p1->b, a = p1->a;

    uv_bound_set(u0, u1 - uw, v0, v1 - vw);
    ensure_room(6);
    uint32_t first = s_nverts;
    put_vert(xa, ya, z, 1.0f, ua, va, fog, r, g, bb_, a);
    put_vert(xb, ya, z, 1.0f, ub, va, fog, r, g, bb_, a);
    put_vert(xa, yb, z, 1.0f, ua, vb, fog, r, g, bb_, a);
    put_vert(xb, ya, z, 1.0f, ub, va, fog, r, g, bb_, a);
    put_vert(xb, yb, z, 1.0f, ub, vb, fog, r, g, bb_, a);
    put_vert(xa, yb, z, 1.0f, ua, vb, fog, r, g, bb_, a);
    uv_bound_clear();
    append(&b, first, 6);
    s_cnt_spr++;
    return 1;
}

static int hook_line(const GeVtx *A, const GeVtx *B, int persp) {
    if (!s_ready) return 0;
    Batch b;
    state_get(persp, 1, &b);
    if (!begin_target()) return 1;

    float dx = B->x - A->x, dy = B->y - A->y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-6f) { dx = 1.0f; dy = 0.0f; len = 1.0f; }
    float nx = -dy / len * 0.5f, ny = dx / len * 0.5f;
    float ex = dx / len * 0.5f, ey = dy / len * 0.5f;
    float ax = A->x + 0.5f, ay = A->y + 0.5f;
    float bx = B->x + 0.5f, by = B->y + 0.5f;

    int flat = !s_ge->shade_gouraud;
    const GeVtx *cA = flat ? B : A;

    ensure_room(6);
    uint32_t first = s_nverts;
    put_vert(ax - ex - nx, ay - ey - ny, A->z, A->rw, A->u, A->v, A->fog, cA->r, cA->g, cA->b, cA->a);
    put_vert(ax - ex + nx, ay - ey + ny, A->z, A->rw, A->u, A->v, A->fog, cA->r, cA->g, cA->b, cA->a);
    put_vert(bx + ex - nx, by + ey - ny, B->z, B->rw, B->u, B->v, B->fog, B->r, B->g, B->b, B->a);
    put_vert(ax - ex + nx, ay - ey + ny, A->z, A->rw, A->u, A->v, A->fog, cA->r, cA->g, cA->b, cA->a);
    put_vert(bx + ex + nx, by + ey + ny, B->z, B->rw, B->u, B->v, B->fog, B->r, B->g, B->b, B->a);
    put_vert(bx + ex - nx, by + ey - ny, B->z, B->rw, B->u, B->v, B->fog, B->r, B->g, B->b, B->a);
    append(&b, first, 6);
    s_cnt_line++;
    return 1;
}

static int hook_point(const GeVtx *A, int persp) {
    if (!s_ready) return 0;
    Batch b;
    state_get(persp, 1, &b);
    if (!begin_target()) return 1;
    float x0 = floorf(A->x), y0 = floorf(A->y);
    ensure_room(6);
    uint32_t first = s_nverts;
    put_vert(x0,        y0,        A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0 + 1.0f, y0,        A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0,        y0 + 1.0f, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0 + 1.0f, y0,        A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0 + 1.0f, y0 + 1.0f, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0,        y0 + 1.0f, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    append(&b, first, 6);
    s_cnt_line++;
    return 1;
}

static void hook_vram_dirty(uint32_t addr, uint32_t bytes) {
    if (!s_ready) return;
    if ((addr & 0x0F000000u) != 0x04000000u) return;
    uint32_t a0 = addr & 0x001FFFFFu;
    for (int i = 0; i < MAX_TGT; i++) {
        Target *t = &s_tgts[i];
        if (!t->used || !t->gpu_valid) continue;
        uint32_t bpp_t = t->fmt == 3 ? 4 : 2;
        uint32_t flen = t->stride * FB_H * bpp_t;
        if (a0 < t->fba + flen && t->fba < a0 + bytes) {
            if (t == s_cur && s_nbatch) submit_pending();
            t->gpu_valid = 0;
            s_cnt_dirty++;
        }
    }
}

static Target *target_containing(uint32_t a) {
    for (int i = 0; i < MAX_TGT; i++) {
        Target *t = &s_tgts[i];
        if (!t->used) continue;
        uint32_t bpp_t = t->fmt == 3 ? 4 : 2;
        if (a >= t->fba && a < t->fba + t->stride * FB_H * bpp_t) return t;
    }
    return NULL;
}

static int hook_xfer(uint32_t startdata) {
    if (!s_ready) return 0;
    uint32_t srcBase = (s_ge->xf_src & 0xFFFFF0u) | ((s_ge->xf_srcw & 0xFF0000u) << 8);
    uint32_t dstBase = (s_ge->xf_dst & 0xFFFFF0u) | ((s_ge->xf_dstw & 0xFF0000u) << 8);
    if ((srcBase & 0x0F000000u) != 0x04000000u || (dstBase & 0x0F000000u) != 0x04000000u)
        return 0;
    uint32_t srcStride = s_ge->xf_srcw & 0x7F8u;
    uint32_t dstStride = s_ge->xf_dstw & 0x7F8u;
    uint32_t bpp = (startdata & 1) ? 4 : 2;
    Target *src = target_containing(srcBase & 0x001FFFFFu);
    Target *dst = target_containing(dstBase & 0x001FFFFFu);
    if (!src || !dst || src == dst || !src->gpu_valid) return 0;
    if (src->scale != dst->scale) return 0;
    uint32_t bpp_s = src->fmt == 3 ? 4 : 2;
    if (src->fmt != dst->fmt || bpp_s != bpp) return 0;
    if (src->stride != srcStride || dst->stride != dstStride) return 0;
    uint32_t soff = ((srcBase & 0x001FFFFFu) - src->fba) / bpp;
    uint32_t doff = ((dstBase & 0x001FFFFFu) - dst->fba) / bpp;
    uint32_t sx = (s_ge->xf_spos & 0x3FFu) + soff % src->stride;
    uint32_t sy = ((s_ge->xf_spos >> 10) & 0x3FFu) + soff / src->stride;
    uint32_t dx = (s_ge->xf_dpos & 0x3FFu) + doff % dst->stride;
    uint32_t dy = ((s_ge->xf_dpos >> 10) & 0x3FFu) + doff / dst->stride;
    uint32_t w = (s_ge->xf_size & 0x3FFu) + 1, h = ((s_ge->xf_size >> 10) & 0x3FFu) + 1;
    if (sx + w > FB_W || dx + w > FB_W || sy + h > FB_H || dy + h > FB_H) return 0;
    if (!dst->gpu_valid && !target_upload(dst)) return 0;
    if (s_nbatch && (s_cur == src || s_cur == dst)) submit_pending();
    if (!cmd_begin()) return 0;
    to_layout(s_cmd, src->img, VK_IMAGE_ASPECT_COLOR_BIT, &src->layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to_layout(s_cmd, dst->img, VK_IMAGE_ASPECT_COLOR_BIT, &dst->layout,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const uint32_t xs = src->scale;
    VkImageCopy ic = {0};
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ic.srcSubresource.layerCount = 1;
    ic.dstSubresource = ic.srcSubresource;
    ic.srcOffset.x = (int32_t)(sx * xs); ic.srcOffset.y = (int32_t)(sy * xs);
    ic.dstOffset.x = (int32_t)(dx * xs); ic.dstOffset.y = (int32_t)(dy * xs);
    ic.extent.width = w * xs; ic.extent.height = h * xs; ic.extent.depth = 1;
    vkCmdCopyImage(s_cmd, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    to_layout(s_cmd, dst->img, VK_IMAGE_ASPECT_COLOR_BIT, &dst->layout,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!cmd_submit()) return 0;
    dst->render_gen++;
    target_drew(dst, dy, dy + h);
    dst->lru = s_lru++;
    s_cnt_xferblit++;
    return 1;
}

static void gegpu_flush(const char *reason) {
    if (!s_ready) return;
    if (reason && strcmp(reason, "listend") == 0) { stats_tick(); return; }
    if (reason && strcmp(reason, "xfersrc") == 0) {
        submit_pending();
        uint32_t srcBase = (s_ge->xf_src & 0xFFFFF0u) | ((s_ge->xf_srcw & 0xFF0000u) << 8);
        if ((srcBase & 0x0F000000u) != 0x04000000u) return;
        uint32_t srcStride = s_ge->xf_srcw & 0x7F8u;
        uint32_t srcY = (s_ge->xf_spos >> 10) & 0x3FFu;
        uint32_t h = ((s_ge->xf_size >> 10) & 0x3FFu) + 1;
        uint32_t bpp = s_ge->xf_bpp ? s_ge->xf_bpp : 2u;
        uint32_t pitch = (srcStride ? srcStride : 512u) * bpp;
        uint32_t a0 = (srcBase & 0x001FFFFFu) + srcY * pitch;
        uint32_t bytes = h * pitch;
        s_rb_reason = RB_XFERSRC;
        for (int i = 0; i < MAX_TGT; i++)
            if (s_tgts[i].used) target_readback_range(&s_tgts[i], a0, bytes);
        s_rb_reason = RB_CLUT;
        return;
    }
    if (reason && strcmp(reason, "loadclut") == 0) {
        uint32_t ca = s_ge->clut_addr & 0x001FFFFFu;
        for (int i = 0; i < MAX_TGT; i++)
            if (s_tgts[i].used) target_readback_range(&s_tgts[i], ca, CLUT_MAX_LOAD_BYTES);
        return;
    }
    submit_pending();
}

static int gegpu_present(uint32_t fbaddr, int fmt, uint32_t stride) {
    (void)stride;
    if (!s_ready) return -1;
    uint32_t fba = fbaddr & 0x001FFFFFu;
    Target *t = NULL;
    for (int i = 0; i < MAX_TGT; i++)
        if (s_tgts[i].used && s_tgts[i].fba == fba) { t = &s_tgts[i]; break; }
    if (!t || !t->gpu_valid || (int)t->fmt != (fmt & 3)) { s_cnt_present_cpu++; return -1; }
    if (t == s_cur && s_nbatch) submit_pending();

    static int safe = -1;
    if (safe < 0) { const char *sv = getenv("SR_GPU_SAFE"); safe = (sv && sv[0] && strcmp(sv, "0") != 0) ? 1 : 0; }
    if (safe) {
        s_rb_reason = RB_PRESENT;
        target_readback(t);
        s_rb_reason = RB_CLUT;
    }

    if (t->layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        if (!cmd_begin()) return -1;
        to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (!cmd_submit()) return -1;
    }
    /* Optional pixel-sequence regression trace. Disabled in ordinary play. */
    {
        static int trace_checked;
        static FILE *trace;
        static unsigned long long sequence;
        if (!trace_checked) {
            const char *path = getenv("PSPRECO_TEST_VIDEO_HASH");
            trace_checked = 1;
            if (path && *path) trace = fopen(path, "w");
            if (trace) fprintf(trace, "sequence,movie_frame,buffer,pixel_hash\n");
        }
        if (trace && target_readback(t)) {
            uint32_t hash = 2166136261u;
            uint32_t bpp = t->fmt == 3 ? 4u : 2u;
            const uint8_t *pixels = g_ram + 0x04000000u + t->fba;
            for (uint32_t y = 0; y < FB_H; y++)
                for (uint32_t x = 0; x < DRAW_W * bpp; x++)
                    hash = (hash ^ pixels[y * t->stride * bpp + x]) * 16777619u;
            fprintf(trace, "%llu,%u,%u,%08x\n", ++sequence,
                    mem_r32(g_ram, 0x08b906a4u), t->fba, hash);
            fflush(trace);
        }
    }
    s_cnt_present_gpu++;
    stats_tick();
    return present_image((void *)t->img, (int)(DRAW_W * t->scale),
                         (int)(FB_H * t->scale));
}

static const GeGpuHooks k_hooks = {
    hook_tri, hook_sprite, hook_line, hook_point, gegpu_flush, hook_vram_dirty, hook_xfer,
};

int gegpu_init(void) {
    PresentVkInfo vi;
    if (!present_get_vk(&vi)) { fprintf(stderr, "gegpu: presenter has no Vulkan device\n"); return 0; }
    s_pdev  = (VkPhysicalDevice)vi.physical;
    s_dev   = (VkDevice)vi.device;
    s_queue = (VkQueue)vi.queue;
    s_ge    = ge_state_ptr();
    s_zbuf  = ge_zbuf_ptr();
    {
        const char *lg = getenv("SR_GPU_LOG");
        s_log = (lg && lg[0] && strcmp(lg, "0") != 0) ? 1 : 0;
    }

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = vi.queue_family;
    VKC(vkCreateCommandPool(s_dev, &cpi, NULL, &s_pool));
    VkCommandBuffer cbs[RING];
    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = s_pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = RING;
    VKC(vkAllocateCommandBuffers(s_dev, &cbi, cbs));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (int i = 0; i < RING; i++) {
        s_ring[i].cmd = cbs[i];
        VKC(vkCreateFence(s_dev, &fci, NULL, &s_ring[i].fence));
    }
    s_cmd = s_ring[1 % RING].cmd;

    VkAttachmentDescription at[2] = {0};
    at[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    at[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    at[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    at[1] = at[0];
    at[1].format = VK_FORMAT_D16_UNORM;
    at[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference zref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &zref;
    VkSubpassDependency dep[2] = {0};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                        | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                        | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                         | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                        | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

    VkRenderPassCreateInfo rpc = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpc.attachmentCount = 2; rpc.pAttachments = at;
    rpc.subpassCount = 1; rpc.pSubpasses = &sub;
    rpc.dependencyCount = 2; rpc.pDependencies = dep;
    VKC(vkCreateRenderPass(s_dev, &rpc, NULL, &s_rp));

    if (!make_buffer((VkDeviceSize)RING * MAX_VERTS * sizeof(GpuVert),
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     &s_vbuf, &s_vbuf_m, (void **)&s_vmap)) return 0;
    s_vstage = (GpuVert *)malloc(MAX_VERTS * sizeof(GpuVert));
    if (!s_vstage) return 0;
    for (int i = 0; i < XFER_RING; i++)
        if (!make_buffer(XFER_SIZE,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         &s_xfer[i].buf, &s_xfer[i].mem, &s_xfer[i].map)) return 0;

    VkDescriptorSetLayoutBinding db = {0};
    db.binding = 0; db.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    db.descriptorCount = 1; db.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dlc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dlc.bindingCount = 1; dlc.pBindings = &db;
    VKC(vkCreateDescriptorSetLayout(s_dev, &dlc, NULL, &s_dlayout));

    VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEX };
    VkDescriptorPoolCreateInfo dpc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpc.maxSets = MAX_TEX; dpc.poolSizeCount = 1; dpc.pPoolSizes = &dps;
    dpc.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    VKC(vkCreateDescriptorPool(s_dev, &dpc, NULL, &s_dpool_tex));
    VkDescriptorPoolSize dpsf = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * MAX_TGT + 3 };
    dpc.maxSets = 2 * MAX_TGT + 3; dpc.pPoolSizes = &dpsf;
    dpc.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    VKC(vkCreateDescriptorPool(s_dev, &dpc, NULL, &s_dpool_fix));

    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushPC) };
    VkPipelineLayoutCreateInfo plc = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plc.setLayoutCount = 1; plc.pSetLayouts = &s_dlayout;
    plc.pushConstantRangeCount = 1; plc.pPushConstantRanges = &pcr;
    VKC(vkCreatePipelineLayout(s_dev, &plc, NULL, &s_playout));

    VkShaderModuleCreateInfo smc = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smc.codeSize = sizeof(k_vert_spv); smc.pCode = k_vert_spv;
    VKC(vkCreateShaderModule(s_dev, &smc, NULL, &s_vs));
    smc.codeSize = sizeof(k_frag_spv); smc.pCode = k_frag_spv;
    VKC(vkCreateShaderModule(s_dev, &smc, NULL, &s_fs));

    {
        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_NEAREST; sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VKC(vkCreateSampler(s_dev, &sci, NULL, &s_smp_n));
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        VKC(vkCreateSampler(s_dev, &sci, NULL, &s_smp_l));
    }

    {
        uint32_t white = 0xFFFFFFFFu;
        TexLevel lv = { &white, 1, 1 };
        VkSamplerCreateInfo wsci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        if (!make_image_mip(1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                            | VK_IMAGE_USAGE_SAMPLED_BIT, &s_white, &s_white_mem))
            return 0;
        if (!tex_upload(s_white, &lv, 1, 1)) return 0;
        if (!make_view_mip(s_white, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                           1, &s_white_view)) return 0;
        wsci.magFilter = VK_FILTER_NEAREST;
        wsci.minFilter = VK_FILTER_NEAREST;
        wsci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        wsci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        wsci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        wsci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VKC(vkCreateSampler(s_dev, &wsci, NULL, &s_white_smp));
        s_white_set = make_descriptor(s_white_view, s_white_smp, s_dpool_fix);
        if (!s_white_set) return 0;
    }

    if (!make_image(FB_W, FB_H, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    &s_snapimg, &s_snapimg_mem)) return 0;
    if (!make_view(s_snapimg, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, &s_snap_view)) return 0;
    s_snap_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    s_snap_n = make_descriptor(s_snap_view, s_smp_n, s_dpool_fix);
    s_snap_l = make_descriptor(s_snap_view, s_smp_l, s_dpool_fix);
    if (!s_snap_n || !s_snap_l) return 0;

    texrep_init();

    s_ready = 1;
    ge_set_gpu_hooks(&k_hooks);
    ge_gpu_present_fn = gegpu_present;
    fprintf(stderr, "gegpu: full GPU GE active (persistent targets, no software fallback)\n");
    return 1;
}

void gegpu_shutdown(void) {
    if (!s_ready) return;
    submit_pending();
    ge_set_gpu_hooks(NULL);
    ge_gpu_present_fn = NULL;
    s_ready = 0;
    gpu_sync(); vkDeviceWaitIdle(s_dev);

    for (int i = 0; i < s_tex_n; i++) tex_free_entry(&s_tex[i]);
    s_tex_n = 0;
    s_tex_bytes = 0;
    texrep_shutdown();

    for (int i = 0; i < MAX_TGT; i++) target_destroy(&s_tgts[i]);

    for (int i = 0; i < MAX_DEP; i++) {
        DepthEnt *d = &s_deps[i];
        if (!d->used) continue;
        if (d->view) vkDestroyImageView(s_dev, d->view, NULL);
        if (d->img) vkDestroyImage(s_dev, d->img, NULL);
        if (d->mem) vkFreeMemory(s_dev, d->mem, NULL);
        memset(d, 0, sizeof(*d));
    }

    for (int i = 0; i < s_pipe_n; i++)
        if (s_pipes[i].pipe) vkDestroyPipeline(s_dev, s_pipes[i].pipe, NULL);
    s_pipe_n = 0;

    if (s_snap_view) vkDestroyImageView(s_dev, s_snap_view, NULL);
    if (s_snapimg) vkDestroyImage(s_dev, s_snapimg, NULL);
    if (s_snapimg_mem) vkFreeMemory(s_dev, s_snapimg_mem, NULL);
    if (s_white_smp) vkDestroySampler(s_dev, s_white_smp, NULL);
    if (s_white_view) vkDestroyImageView(s_dev, s_white_view, NULL);
    if (s_white) vkDestroyImage(s_dev, s_white, NULL);
    if (s_white_mem) vkFreeMemory(s_dev, s_white_mem, NULL);
    if (s_smp_n) vkDestroySampler(s_dev, s_smp_n, NULL);
    if (s_smp_l) vkDestroySampler(s_dev, s_smp_l, NULL);

    for (int i = 0; i < XFER_RING; i++) {
        if (s_xfer[i].buf) vkDestroyBuffer(s_dev, s_xfer[i].buf, NULL);
        if (s_xfer[i].mem) vkFreeMemory(s_dev, s_xfer[i].mem, NULL);
    }
    if (s_texstage) vkDestroyBuffer(s_dev, s_texstage, NULL);
    if (s_texstage_mem) vkFreeMemory(s_dev, s_texstage_mem, NULL);
    s_texstage = VK_NULL_HANDLE;
    s_texstage_mem = VK_NULL_HANDLE;
    s_texstage_map = NULL;
    s_texstage_size = 0;
    if (s_vbuf) vkDestroyBuffer(s_dev, s_vbuf, NULL);
    if (s_vbuf_m) vkFreeMemory(s_dev, s_vbuf_m, NULL);
    free(s_vstage);
    s_vstage = NULL;

    if (s_dpool_tex) vkDestroyDescriptorPool(s_dev, s_dpool_tex, NULL);
    if (s_dpool_fix) vkDestroyDescriptorPool(s_dev, s_dpool_fix, NULL);
    if (s_playout) vkDestroyPipelineLayout(s_dev, s_playout, NULL);
    if (s_dlayout) vkDestroyDescriptorSetLayout(s_dev, s_dlayout, NULL);
    if (s_vs) vkDestroyShaderModule(s_dev, s_vs, NULL);
    if (s_fs) vkDestroyShaderModule(s_dev, s_fs, NULL);
    if (s_rp) vkDestroyRenderPass(s_dev, s_rp, NULL);

    for (int i = 0; i < RING; i++)
        if (s_ring[i].fence) vkDestroyFence(s_dev, s_ring[i].fence, NULL);
    if (s_pool) vkDestroyCommandPool(s_dev, s_pool, NULL);
}

#else
typedef int pspreco_ge_gpu_vk_has_no_headless_contents;
#endif
