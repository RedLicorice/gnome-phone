/* The 2D renderer (see gfx.h). */
#include "gfx.h"

#include <glib.h>
#include <math.h>
#include <string.h>

#include "quad_vert.h"
#include "quad_frag.h"
#include "blur_down_frag.h"
#include "blur_up_frag.h"
#include "stretch_frag.h"

#define INST_BYTES    (4u << 20)     /* per frame in flight: ~43000 quads */
#define STAGING_BYTES (8u << 20)     /* per frame in flight; larger uploads go their own way */
#define MAX_SETS      2048

typedef struct {
  float vp[2];
  float pad[2];
  float p[8];
} Push;

typedef struct {
  float m[4], t[2];
  float alpha;
  float clip[4];   /* screen space, x1 <= x0 = none */
} State;

typedef struct {
  GfxTex *tex;
  int     x, y, w, h;
  guint8 *data;      /* tightly packed */
  int     bpp;
} Upload;

typedef struct {
  GfxTex  *tex;
  uint32_t frame;    /* free once frame_index passes this + VK_FRAMES */
} Dead;

static struct {
  Vk                   *vk;
  VkDescriptorSetLayout dsl;
  VkDescriptorPool      pool;
  VkPipelineLayout      layout;
  VkPipeline            pipes[GFX_N_PIPES];
  VkSampler             sampler;
  VkShaderModule        vert;

  VkBuffer              inst_buf[VK_FRAMES];
  VkDeviceMemory        inst_mem[VK_FRAMES];
  GfxInst              *inst_map[VK_FRAMES];
  uint32_t              inst_used;
  VkDeviceSize          stage_used;

  VkBuffer              stage_buf[VK_FRAMES];
  VkDeviceMemory        stage_mem[VK_FRAMES];
  guint8               *stage_map[VK_FRAMES];

  GArray               *uploads;    /* Upload */
  GArray               *dead;       /* Dead */
  GfxTex               *white;
  GfxTex               *blur_targets[4][6];
  int                   blur_size[4][2];

  /* the frame being recorded */
  VkCommandBuffer       cmd;
  bool                  in_pass;
  int                   pass_w, pass_h;
  Push                  push;
  GfxPipe               bound_pipe;
  GfxTex               *batch_tex;
  GfxPipe               batch_pipe;
  uint32_t              batch_first, batch_count;
  bool                  push_dirty;

  State                 state;
  GArray               *stack;      /* State */
} G;

Vk *
gfx_vk (void)
{
  return G.vk;
}

static VkShaderModule
shader (const uint32_t *code, size_t size)
{
  VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
  VkShaderModule m = VK_NULL_HANDLE;
  vkCreateShaderModule (G.vk->dev, &ci, NULL, &m);
  return m;
}

static VkPipeline
make_pipeline (const uint32_t *frag_code, size_t frag_size, bool blend)
{
  VkShaderModule frag = shader (frag_code, frag_size);
  VkPipelineShaderStageCreateInfo stages[2] = {
    { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
      .module = G.vert, .pName = "main" },
    { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
      .module = frag, .pName = "main" },
  };
  VkVertexInputBindingDescription bind = { 0, sizeof (GfxInst), VK_VERTEX_INPUT_RATE_INSTANCE };
  VkVertexInputAttributeDescription attrs[6];
  for (int i = 0; i < 6; i++)
    attrs[i] = (VkVertexInputAttributeDescription) { i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16 };
  VkPipelineVertexInputStateCreateInfo vi = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind,
    .vertexAttributeDescriptionCount = 6, .pVertexAttributeDescriptions = attrs,
  };
  VkPipelineInputAssemblyStateCreateInfo ia = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  VkPipelineViewportStateCreateInfo vp = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1,
  };
  VkPipelineRasterizationStateCreateInfo rs = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
    .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f,
  };
  VkPipelineMultisampleStateCreateInfo ms = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  VkPipelineColorBlendAttachmentState cba = {
    .blendEnable = blend,
    .srcColorBlendFactor = VK_BLEND_FACTOR_ONE, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .colorBlendOp = VK_BLEND_OP_ADD,
    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .alphaBlendOp = VK_BLEND_OP_ADD,
    .colorWriteMask = 0xf,
  };
  VkPipelineColorBlendStateCreateInfo cb = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba,
  };
  VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
  VkPipelineDynamicStateCreateInfo ds = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn,
  };
  VkGraphicsPipelineCreateInfo ci = {
    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
    .stageCount = 2, .pStages = stages,
    .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
    .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds,
    .layout = G.layout,
    .renderPass = G.vk->rp_present,     /* the offscreen pass has the same format: compatible */
  };
  VkPipeline p = VK_NULL_HANDLE;
  if (vkCreateGraphicsPipelines (G.vk->dev, VK_NULL_HANDLE, 1, &ci, NULL, &p) != VK_SUCCESS)
    g_warning ("gfx: pipeline creation failed");
  vkDestroyShaderModule (G.vk->dev, frag, NULL);
  return p;
}

static void
state_identity (State *s)
{
  *s = (State) { .m = { 1, 0, 0, 1 }, .alpha = 1, .clip = { 0, 0, 0, 0 } };
}

bool
gfx_init (Vk *vk)
{
  G.vk = vk;
  VkDescriptorSetLayoutBinding b = {
    .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
  };
  VkDescriptorSetLayoutCreateInfo dli = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b,
  };
  VK_CHECK (vkCreateDescriptorSetLayout (vk->dev, &dli, NULL, &G.dsl));
  VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_SETS };
  VkDescriptorPoolCreateInfo dpi = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    .maxSets = MAX_SETS, .poolSizeCount = 1, .pPoolSizes = &ps,
  };
  VK_CHECK (vkCreateDescriptorPool (vk->dev, &dpi, NULL, &G.pool));
  VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof (Push) };
  VkPipelineLayoutCreateInfo pli = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1, .pSetLayouts = &G.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr,
  };
  VK_CHECK (vkCreatePipelineLayout (vk->dev, &pli, NULL, &G.layout));
  VkSamplerCreateInfo sci = {
    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
    .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .maxLod = 0,
  };
  VK_CHECK (vkCreateSampler (vk->dev, &sci, NULL, &G.sampler));

  G.vert = shader (quad_vert, sizeof quad_vert);
  G.pipes[GFX_PIPE_QUAD] = make_pipeline (quad_frag, sizeof quad_frag, true);
  G.pipes[GFX_PIPE_BLUR_DOWN] = make_pipeline (blur_down_frag, sizeof blur_down_frag, false);
  G.pipes[GFX_PIPE_BLUR_UP] = make_pipeline (blur_up_frag, sizeof blur_up_frag, false);
  G.pipes[GFX_PIPE_STRETCH] = make_pipeline (stretch_frag, sizeof stretch_frag, true);

  for (int i = 0; i < VK_FRAMES; i++) {
    VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!vk_alloc_buffer (vk, INST_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, hv, &G.inst_buf[i], &G.inst_mem[i]) ||
        !vk_alloc_buffer (vk, STAGING_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hv, &G.stage_buf[i], &G.stage_mem[i]))
      return false;
    vkMapMemory (vk->dev, G.inst_mem[i], 0, INST_BYTES, 0, (void **) &G.inst_map[i]);
    vkMapMemory (vk->dev, G.stage_mem[i], 0, STAGING_BYTES, 0, (void **) &G.stage_map[i]);
  }
  G.uploads = g_array_new (FALSE, FALSE, sizeof (Upload));
  G.dead = g_array_new (FALSE, FALSE, sizeof (Dead));
  G.stack = g_array_new (FALSE, FALSE, sizeof (State));
  state_identity (&G.state);
  guint32 white = 0xffffffff;
  G.white = gfx_tex_from_bgra (1, 1, &white, 4);
  return G.white != NULL;
}

GfxTex *
gfx_white (void)
{
  return G.white;
}

/* ---- textures ------------------------------------------------------------------------------------------------- */

static GfxTex *
tex_new (int w, int h, VkFormat format, VkImageUsageFlags usage)
{
  Vk *vk = G.vk;
  GfxTex *t = g_new0 (GfxTex, 1);
  t->w = w;
  t->h = h;
  t->format = format;
  t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageCreateInfo ici = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = { w, h, 1 },
    .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
    .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  if (vkCreateImage (vk->dev, &ici, NULL, &t->image) != VK_SUCCESS)
    goto fail;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements (vk->dev, t->image, &req);
  VkMemoryAllocateInfo mai = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
    .memoryTypeIndex = vk_memory_type (vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
  };
  if (vkAllocateMemory (vk->dev, &mai, NULL, &t->mem) != VK_SUCCESS)
    goto fail;
  vkBindImageMemory (vk->dev, t->image, t->mem, 0);
  VkComponentMapping sw = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
  VkImageViewCreateInfo vci = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
    .format = format, .components = sw, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
  };
  if (vkCreateImageView (vk->dev, &vci, NULL, &t->view) != VK_SUCCESS)
    goto fail;
  VkDescriptorSetAllocateInfo dai = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = G.pool, .descriptorSetCount = 1, .pSetLayouts = &G.dsl,
  };
  if (vkAllocateDescriptorSets (vk->dev, &dai, &t->set) != VK_SUCCESS)
    goto fail;
  VkDescriptorImageInfo dii = { G.sampler, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
  VkWriteDescriptorSet wds = {
    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = t->set, .descriptorCount = 1,
    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii,
  };
  vkUpdateDescriptorSets (vk->dev, 1, &wds, 0, NULL);
  return t;
fail:
  g_warning ("gfx: cannot make a %dx%d texture", w, h);
  gfx_tex_free (t);
  return NULL;
}

GfxTex *
gfx_tex_new (int w, int h, VkFormat format)
{
  return tex_new (w, h, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
}

GfxTex *
gfx_target_new (int w, int h)
{
  GfxTex *t = tex_new (w, h, G.vk->format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  if (!t)
    return NULL;
  VkFramebufferCreateInfo fci = {
    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = G.vk->rp_offscreen,
    .attachmentCount = 1, .pAttachments = &t->view, .width = w, .height = h, .layers = 1,
  };
  vkCreateFramebuffer (G.vk->dev, &fci, NULL, &t->fb);
  return t;
}

static void
tex_destroy (GfxTex *t)
{
  Vk *vk = G.vk;
  if (t->fb) vkDestroyFramebuffer (vk->dev, t->fb, NULL);
  if (t->set) vkFreeDescriptorSets (vk->dev, G.pool, 1, &t->set);
  if (t->view) vkDestroyImageView (vk->dev, t->view, NULL);
  if (t->image) vkDestroyImage (vk->dev, t->image, NULL);
  if (t->mem) vkFreeMemory (vk->dev, t->mem, NULL);
  g_free (t);
}

void
gfx_tex_free (GfxTex *t)
{
  if (!t)
    return;
  for (guint i = 0; i < G.uploads->len; i++) {       /* a pending upload into it goes too */
    Upload *u = &g_array_index (G.uploads, Upload, i);
    if (u->tex == t) {
      g_free (u->data);
      g_array_remove_index (G.uploads, i--);
    }
  }
  Dead d = { t, G.vk->frame_index };
  g_array_append_val (G.dead, d);
}

static void
barrier (VkCommandBuffer cmd, GfxTex *t, VkImageLayout to, VkAccessFlags src_access, VkAccessFlags dst_access,
         VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
  VkImageMemoryBarrier b = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
    .srcAccessMask = src_access, .dstAccessMask = dst_access,
    .oldLayout = t->layout, .newLayout = to,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .image = t->image, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
  };
  vkCmdPipelineBarrier (cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
  t->layout = to;
}

/* Copies of `data` (tightly packed) from `buf` at `off` into t, recorded into cmd */
static void
record_copy (VkCommandBuffer cmd, GfxTex *t, VkBuffer buf, VkDeviceSize off, int x, int y, int w, int h)
{
  barrier (cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy r = {
    .bufferOffset = off,
    .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
    .imageOffset = { x, y, 0 }, .imageExtent = { w, h, 1 },
  };
  vkCmdCopyBufferToImage (cmd, buf, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
  barrier (cmd, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

static int
bpp_of (VkFormat f)
{
  return f == VK_FORMAT_R8_UNORM ? 1 : 4;
}

void
gfx_tex_write (GfxTex *t, int x, int y, int w, int h, const void *pixels, int stride)
{
  int bpp = bpp_of (t->format);
  gsize size = (gsize) w * h * bpp;
  if (size > STAGING_BYTES / 2) {
    /* too big for a frame's staging space (a wallpaper): its own buffer, now, and wait for it */
    Vk *vk = G.vk;
    VkBuffer buf;
    VkDeviceMemory mem;
    if (!vk_alloc_buffer (vk, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &buf, &mem))
      return;
    guint8 *map;
    vkMapMemory (vk->dev, mem, 0, size, 0, (void **) &map);
    for (int r = 0; r < h; r++)
      memcpy (map + (gsize) r * w * bpp, (const guint8 *) pixels + (gsize) r * stride, (gsize) w * bpp);
    vkUnmapMemory (vk->dev, mem);
    VkCommandBuffer cmd = vk_oneshot_begin (vk);
    record_copy (cmd, t, buf, 0, x, y, w, h);
    vk_oneshot_end (vk, cmd);
    vkDestroyBuffer (vk->dev, buf, NULL);
    vkFreeMemory (vk->dev, mem, NULL);
    return;
  }
  Upload u = { t, x, y, w, h, g_malloc (size), bpp };
  for (int r = 0; r < h; r++)
    memcpy (u.data + (gsize) r * w * bpp, (const guint8 *) pixels + (gsize) r * stride, (gsize) w * bpp);
  g_array_append_val (G.uploads, u);
}

GfxTex *
gfx_tex_from_bgra (int w, int h, const void *pixels, int stride)
{
  GfxTex *t = gfx_tex_new (w, h, G.vk->format == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_UNORM
                                                                          : VK_FORMAT_B8G8R8A8_UNORM);
  if (!t)
    return NULL;
  if (G.vk->format == VK_FORMAT_R8G8B8A8_UNORM) {
    /* the cairo order is BGRA in memory; swap for an RGBA device */
    guint8 *sw = g_malloc ((gsize) w * h * 4);
    for (int y = 0; y < h; y++) {
      const guint8 *s = (const guint8 *) pixels + (gsize) y * stride;
      guint8 *d = sw + (gsize) y * w * 4;
      for (int x = 0; x < w; x++, s += 4, d += 4) {
        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
      }
    }
    gfx_tex_write (t, 0, 0, w, h, sw, w * 4);
    g_free (sw);
  } else {
    gfx_tex_write (t, 0, 0, w, h, pixels, stride);
  }
  return t;
}

GfxTex *
gfx_tex_from_gray (int w, int h, const void *pixels, int stride)
{
  GfxTex *t = gfx_tex_new (w, h, VK_FORMAT_R8_UNORM);
  if (t)
    gfx_tex_write (t, 0, 0, w, h, pixels, stride);
  return t;
}

static void
flush_uploads (void)
{
  uint32_t slot = G.vk->frame_index % VK_FRAMES;
  VkDeviceSize off = G.stage_used;
  guint done = 0;
  for (; done < G.uploads->len; done++) {
    Upload *u = &g_array_index (G.uploads, Upload, done);
    gsize size = (gsize) u->w * u->h * u->bpp;
    off = (off + 15) & ~(VkDeviceSize) 15;
    if (off + size > STAGING_BYTES)
      break;                                  /* the rest next frame */
    memcpy (G.stage_map[slot] + off, u->data, size);
    record_copy (G.cmd, u->tex, G.stage_buf[slot], off, u->x, u->y, u->w, u->h);
    off += size;
    g_free (u->data);
  }
  G.stage_used = off;
  g_array_remove_range (G.uploads, 0, done);
}

static void
reap (void)
{
  for (guint i = 0; i < G.dead->len; i++) {
    Dead *d = &g_array_index (G.dead, Dead, i);
    if (G.vk->frame_index >= d->frame + VK_FRAMES + 1) {
      tex_destroy (d->tex);
      g_array_remove_index (G.dead, i--);
    }
  }
}

/* ---- frames and passes ---------------------------------------------------------------------------------------- */

bool
gfx_frame_begin (void)
{
  G.cmd = vk_frame_begin (G.vk);
  if (!G.cmd)
    return false;
  reap ();
  G.inst_used = 0;
  G.stage_used = 0;
  return true;
}

static void
flush_batch (void)
{
  if (!G.batch_count)
    return;
  if (G.bound_pipe != G.batch_pipe) {
    vkCmdBindPipeline (G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pipes[G.batch_pipe]);
    G.bound_pipe = G.batch_pipe;
  }
  if (G.push_dirty) {
    vkCmdPushConstants (G.cmd, G.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                        sizeof (Push), &G.push);
    G.push_dirty = false;
  }
  vkCmdBindDescriptorSets (G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.layout, 0, 1, &G.batch_tex->set, 0, NULL);
  vkCmdDraw (G.cmd, 6, G.batch_count, 0, G.batch_first);
  G.batch_first += G.batch_count;
  G.batch_count = 0;
}

void
gfx_pass_begin (GfxTex *target, GfxColor c)
{
  Vk *vk = G.vk;
  flush_uploads ();          /* textures written since the last pass, glyphs among them, are ready for this one */
  G.pass_w = target ? target->w : (int) vk->extent.width;
  G.pass_h = target ? target->h : (int) vk->extent.height;
  VkClearValue clear = { .color = { .float32 = { c.r * c.a, c.g * c.a, c.b * c.a, c.a } } };
  VkRenderPassBeginInfo bi = {
    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
    .renderPass = target ? vk->rp_offscreen : vk->rp_present,
    .framebuffer = target ? target->fb : vk->fbs[vk->image_index],
    .renderArea = { { 0, 0 }, { G.pass_w, G.pass_h } },
    .clearValueCount = 1, .pClearValues = &clear,
  };
  vkCmdBeginRenderPass (G.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
  if (target)
    target->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;   /* the pass's final layout */
  VkViewport vp = { 0, 0, G.pass_w, G.pass_h, 0, 1 };
  VkRect2D sc = { { 0, 0 }, { G.pass_w, G.pass_h } };
  vkCmdSetViewport (G.cmd, 0, 1, &vp);
  vkCmdSetScissor (G.cmd, 0, 1, &sc);
  uint32_t slot = vk->frame_index % VK_FRAMES;
  VkDeviceSize off = 0;
  vkCmdBindVertexBuffers (G.cmd, 0, 1, &G.inst_buf[slot], &off);
  G.push.vp[0] = G.pass_w;
  G.push.vp[1] = G.pass_h;
  G.push_dirty = true;
  G.bound_pipe = GFX_N_PIPES;
  G.batch_tex = NULL;
  G.batch_first = G.inst_used;
  G.batch_count = 0;
  G.in_pass = true;
  state_identity (&G.state);
  g_array_set_size (G.stack, 0);
}

void
gfx_pass_end (void)
{
  flush_batch ();
  vkCmdEndRenderPass (G.cmd);
  G.in_pass = false;
}

void
gfx_frame_end (void)
{
  vk_frame_end (G.vk);
  G.cmd = VK_NULL_HANDLE;
}

int gfx_pass_width (void) { return G.pass_w; }
int gfx_pass_height (void) { return G.pass_h; }

/* ---- state ---------------------------------------------------------------------------------------------------- */

void
gfx_save (void)
{
  g_array_append_val (G.stack, G.state);
}

void
gfx_restore (void)
{
  if (G.stack->len) {
    G.state = g_array_index (G.stack, State, G.stack->len - 1);
    g_array_set_size (G.stack, G.stack->len - 1);
  }
}

void
gfx_translate (float x, float y)
{
  State *s = &G.state;
  s->t[0] += s->m[0] * x + s->m[1] * y;
  s->t[1] += s->m[2] * x + s->m[3] * y;
}

void
gfx_scale (float sx, float sy, float px, float py)
{
  gfx_translate (px, py);
  State *s = &G.state;
  s->m[0] *= sx; s->m[2] *= sx;
  s->m[1] *= sy; s->m[3] *= sy;
  gfx_translate (-px, -py);
}

void
gfx_rotate (float radians, float px, float py)
{
  gfx_translate (px, py);
  State *s = &G.state;
  float c = cosf (radians), n = sinf (radians);
  float m0 = s->m[0], m1 = s->m[1], m2 = s->m[2], m3 = s->m[3];
  s->m[0] = m0 * c + m1 * n; s->m[1] = -m0 * n + m1 * c;
  s->m[2] = m2 * c + m3 * n; s->m[3] = -m2 * n + m3 * c;
  gfx_translate (-px, -py);
}

void
gfx_opacity (float a)
{
  G.state.alpha *= a;
}

void
gfx_clip (float x0, float y0, float x1, float y1)
{
  State *s = &G.state;
  /* to screen space (axis-aligned transforms only) */
  float sx0 = s->m[0] * x0 + s->t[0], sy0 = s->m[3] * y0 + s->t[1];
  float sx1 = s->m[0] * x1 + s->t[0], sy1 = s->m[3] * y1 + s->t[1];
  if (sx1 < sx0) { float t = sx0; sx0 = sx1; sx1 = t; }
  if (sy1 < sy0) { float t = sy0; sy0 = sy1; sy1 = t; }
  if (s->clip[2] > s->clip[0]) {
    sx0 = MAX (sx0, s->clip[0]); sy0 = MAX (sy0, s->clip[1]);
    sx1 = MIN (sx1, s->clip[2]); sy1 = MIN (sy1, s->clip[3]);
    if (sx1 <= sx0) sx1 = sx0 + 0.001f;      /* empty, but still "a clip" */
    if (sy1 <= sy0) sy1 = sy0 + 0.001f;
  }
  s->clip[0] = sx0; s->clip[1] = sy0; s->clip[2] = sx1; s->clip[3] = sy1;
}

/* ---- drawing -------------------------------------------------------------------------------------------------- */

GfxInst *
gfx_push (GfxTex *tex, GfxPipe pipe)
{
  if (!tex)
    tex = G.white;
  if (tex != G.batch_tex || pipe != G.batch_pipe) {
    flush_batch ();
    G.batch_tex = tex;
    G.batch_pipe = pipe;
  }
  if ((G.inst_used + 1) * sizeof (GfxInst) > INST_BYTES) {
    static GfxInst scratch;                   /* out of room: draw nothing rather than crash */
    return &scratch;
  }
  uint32_t slot = G.vk->frame_index % VK_FRAMES;
  GfxInst *q = &G.inst_map[slot][G.inst_used++];
  G.batch_count++;
  memset (q, 0, sizeof *q);
  memcpy (q->m, G.state.m, sizeof q->m);
  q->tx = G.state.t[0];
  q->ty = G.state.t[1];
  memcpy (q->clip, G.state.clip, sizeof q->clip);
  q->a = G.state.alpha;   /* callers multiply their colour by this */
  return q;
}

void
gfx_effect_params (const float p[8])
{
  if (memcmp (G.push.p, p, sizeof G.push.p)) {
    flush_batch ();
    memcpy (G.push.p, p, sizeof G.push.p);
    G.push_dirty = true;
  }
}

static GfxInst *
solid (float x, float y, float w, float h, float radius, GfxColor c)
{
  GfxInst *q = gfx_push (NULL, GFX_PIPE_QUAD);
  float a = c.a * q->a;
  q->x = x; q->y = y; q->w = w; q->h = h;
  q->r = c.r * a; q->g = c.g * a; q->b = c.b * a; q->a = a;
  q->u1 = q->v1 = 1;
  q->radius = radius;
  q->mode = GFX_MODE_SOLID;
  return q;
}

void
gfx_rect (float x, float y, float w, float h, float radius, GfxColor c)
{
  solid (x, y, w, h, radius, c);
}

void
gfx_rect_corners (float x, float y, float w, float h, const float r[4], GfxColor c)
{
  GfxInst *q = solid (x, y, w, h, 0, c);
  q->u0 = r[0]; q->v0 = r[1]; q->u1 = r[2]; q->v1 = r[3];
  q->mode = GFX_MODE_CORNERS;
}

void
gfx_ring (float x, float y, float w, float h, float radius, float width, GfxColor c)
{
  GfxInst *q = solid (x, y, w, h, radius, c);
  q->u0 = width; q->v0 = q->u1 = q->v1 = 0;
  q->mode = GFX_MODE_RING;
}

void
gfx_image_uv (GfxTex *t, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
              float radius, float alpha)
{
  GfxInst *q = gfx_push (t, GFX_PIPE_QUAD);
  float a = alpha * q->a;
  q->x = x; q->y = y; q->w = w; q->h = h;
  q->u0 = u0; q->v0 = v0; q->u1 = u1; q->v1 = v1;
  q->r = q->g = q->b = q->a = a;
  q->radius = radius;
  q->mode = GFX_MODE_TEX;
}

void
gfx_mask_uv (GfxTex *t, float x, float y, float w, float h, float u0, float v0, float u1, float v1, GfxColor c)
{
  GfxInst *q = gfx_push (t, GFX_PIPE_QUAD);
  float a = c.a * q->a;
  q->x = x; q->y = y; q->w = w; q->h = h;
  q->u0 = u0; q->v0 = v0; q->u1 = u1; q->v1 = v1;
  q->r = c.r * a; q->g = c.g * a; q->b = c.b * a; q->a = a;
  q->mode = GFX_MODE_MASK;
}

void
gfx_image (GfxTex *t, float x, float y, float w, float h, float radius, float alpha)
{
  gfx_image_uv (t, x, y, w, h, 0, 0, 1, 1, radius, alpha);
}

/* ---- blur ----------------------------------------------------------------------------------------------------- */

GfxTex *
gfx_blur (GfxTex *src, int levels, float offset, int slot)
{
  levels = CLAMP (levels, 1, 5);
  slot = CLAMP (slot, 0, 3);
  if (G.blur_size[slot][0] != src->w || G.blur_size[slot][1] != src->h) {
    for (int i = 0; i < 6; i++) {
      gfx_tex_free (G.blur_targets[slot][i]);
      G.blur_targets[slot][i] = NULL;
    }
    G.blur_size[slot][0] = src->w;
    G.blur_size[slot][1] = src->h;
  }
  GfxTex **tg = G.blur_targets[slot];
  for (int i = 0; i <= levels; i++)
    if (!tg[i] && i > 0)
      tg[i] = gfx_target_new (MAX (1, src->w >> i), MAX (1, src->h >> i));
  GfxTex *from = src;
  /* down: src -> 1/2 -> ... -> 1/2^levels */
  for (int i = 1; i <= levels; i++) {
    gfx_pass_begin (tg[i], (GfxColor) { 0, 0, 0, 0 });
    float p[8] = { 0.5f / from->w * offset, 0.5f / from->h * offset };
    gfx_effect_params (p);
    GfxInst *q = gfx_push (from, GFX_PIPE_BLUR_DOWN);
    q->w = tg[i]->w; q->h = tg[i]->h; q->u1 = q->v1 = 1; q->a = 1;
    gfx_pass_end ();
    from = tg[i];
  }
  /* up: back to 1/2 (the last level up is drawn by the caller, scaled, which is close enough and saves a pass) */
  for (int i = levels - 1; i >= 1; i--) {
    gfx_pass_begin (tg[i], (GfxColor) { 0, 0, 0, 0 });
    float p[8] = { 0.5f / from->w * offset, 0.5f / from->h * offset };
    gfx_effect_params (p);
    GfxInst *q = gfx_push (from, GFX_PIPE_BLUR_UP);
    q->w = tg[i]->w; q->h = tg[i]->h; q->u1 = q->v1 = 1; q->a = 1;
    gfx_pass_end ();
    from = tg[i];
  }
  return from;
}
