/*
 * REDRIVER2's PsyCross renderer (the GR_* layer) on Vulkan.
 *
 * The game draws the PlayStation's way: one VRAM of 1024x512 16-bit texels (RG8 here), a 256x256 colour
 * lookup, and streams of GrVertex triangles whose state (texture format, blend, depth, stencil, clip) is
 * set between draws. This file keeps that state, builds the pipelines it needs the first time they are
 * used, records the draws into the base class's frame command buffer, renders into an offscreen target
 * of the game's resolution, and presents that onto the display with a filtered full screen pass.
 *
 * It runs on the thread that runs the game: the sample's render loop is the game's main (red2_run).
 */
#include "vulkanexamplebase.h"

#include <cstdarg>
#include <time.h>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

// ---- the game's types, as PsyCross lays them out ----
typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned char u_char;
struct RECT16 { short x, y, w, h; };
struct DISPENV_ { RECT16 disp, screen; u_char isinter, isrgb24, pad0, pad1; };
extern DISPENV_ activeDispEnv;
#pragma pack(push, 1)
struct GrVertex {
	float x, y, page, clut;
	float z, scr_h, ofsX, ofsY;
	u_char u, v, bright, dither;
	u_char r, g, b, a;
	char tcx, tcy, _p0, _p1;
};
#pragma pack(pop)
enum BlendMode { BM_NONE, BM_AVERAGE, BM_ADD, BM_SUBTRACT, BM_ADD_QUATER_SOURCE };
enum TexFormat { TF_4_BIT, TF_8_BIT, TF_16_BIT, TF_32_BIT_RGBA };
typedef uint TextureID;
typedef uint ShaderID;

#define VRAM_WIDTH 1024
#define VRAM_HEIGHT 512
#define LUT_WIDTH 256
#define LUT_HEIGHT 256
#define MAX_VERTEX_BUFFER_SIZE (1 << 20)
#define PSX_SCREEN_ASPECT (240.0f / 320.0f)

extern "C" void PsyX_Log(const char* fmt, ...);
#define LOGF(...) PsyX_Log(__VA_ARGS__)

#include "red2_host.h"
static Red2Host* H = nullptr;
#define FRAMES maxConcurrentFrames

// ---- game-visible globals ----
unsigned short vram[VRAM_WIDTH * VRAM_HEIGHT];
static unsigned char rgLUT[LUT_WIDTH * LUT_HEIGHT * 4];
int g_windowWidth = 1920, g_windowHeight = 1080;
int g_cfg_pgxpTextureCorrection = 1, g_cfg_pgxpZBuffer = 1, g_cfg_bilinearFiltering = 0;
float g_cfg_ps5Bloom = 0.45f, g_cfg_ps5SSAO = 0.6f;
int g_cfg_ps5Fog = 0, g_cfg_ps5FogR = 190, g_cfg_ps5FogG = 205, g_cfg_ps5FogB = 225, g_cfg_ps5FogStart = 12, g_cfg_ps5FogEnd = 30;
int g_cfg_ps5HD = 1, g_cfg_ps5Lights = 1, g_cfg_ps5Prepass = 1, g_cfg_ps5RenderScale = 1;
float g_cfg_ps5LightStrength = 1.0f, g_cfg_ps5LightRadius = 1.0f;
int g_cfg_ps5ShadowDebug = 0, g_cfg_ps5ShadowFlip = 0, g_cfg_ps5Shadows = 0, g_cfg_ps5ShadowSize = 4096, g_cfg_ps5ShadowSplits = 0;
float g_cfg_ps5ShadowStrength = 0.5f, g_cfg_ps5Wet = 1;
int g_ps5ModOn = 1;
int g_cfg_farMesh = 0;          // [render] farMesh: regions around the camera baked into the far field (0 = off)
int g_cfg_farMeshNear = 26;     // [render] farMeshNear: cells around the camera left to the game's own drawing
int g_cfg_farMeshDebug = 1;     // [render] farMeshDebug: 1 (default) = no texel is dropped on the far field: the LOD facades have transparent texels that left holes
int g_cfg_farMeshCull = 1;      // [render] farMeshCull: 0 none, 1 clockwise front faces, 2 counter-clockwise
int g_cfg_hdSky = 1;           // [render] hdSky: the panorama sky of the city
int g_cfg_frameInterval = 2;   // vblanks per game frame: 2 = 30 fps, 1 = 60 (config.ini, [render] frameInterval)
static void readFrameInterval()
{
	FILE* f = fopen("/app0/assets/config.ini", "rb");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		int v;
		if (sscanf(line, " frameInterval = %d", &v) == 1 && v >= 1 && v <= 4) g_cfg_frameInterval = v;
		if (sscanf(line, " hdSky = %d", &v) == 1) g_cfg_hdSky = v;
		if (sscanf(line, " farMesh = %d", &v) == 1) g_cfg_farMesh = v;
		if (sscanf(line, " farMeshNear = %d", &v) == 1) g_cfg_farMeshNear = v;
		if (sscanf(line, " farMeshCull = %d", &v) == 1) g_cfg_farMeshCull = v;
		if (sscanf(line, " farMeshDebug = %d", &v) == 1) g_cfg_farMeshDebug = v;
	}
	fclose(f);
	LOGF("vulkan: frame interval %d vblank(s)\n", g_cfg_frameInterval);
}
int g_dbg_wireframeMode = 0, g_dbg_texturelessMode = 0;
int vram_need_update = 1, framebuffer_need_update = 0;
extern "C" {
TextureID g_whiteTexture = 2;
TextureID g_vramTexture = 1;
}
static const TextureID kTexVram = 1, kTexWhite = 2;

// ---- Vulkan objects ----
struct Image {
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	std::vector<VkImageView> layerViews;   // arrays: one view per layer, to render into
	VkFormat format = VK_FORMAT_UNDEFINED;
	uint32_t w = 0, h = 0;
};
struct Tex {
	Image img;
	VkDescriptorSet set = VK_NULL_HANDLE;   // binding 0: this texture, binding 1: the LUT
	bool alive = false;
};
struct Staging { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; void* mapped = nullptr; VkDeviceSize size = 0; };

static VkSampler g_samplerNearest, g_samplerLinear, g_samplerRepeat;
// the far field's own texture store: the level's 4-bit pages (256x256) as 8-bit indices, one layer each, and their
// palettes as rows of 16 colours (row = page * 64 + palette); it does not depend on what is in the game's VRAM
static const int kFarPages = 128, kFarPalRows = 64;
static Image g_farPageImg, g_farPalImg;
static std::vector<unsigned char> g_farPageHave;
static VkDescriptorPool g_descPool;
static VkDescriptorSetLayout g_texSetLayout, g_frameSetLayout, g_presentSetLayout;
static VkPipelineLayout g_pipeLayout, g_presentLayout;
static VkShaderModule g_vs, g_fs, g_pvs, g_pfs;
static Image g_color, g_depth;
static Image g_lut;
static Image g_vramImg;
static const VkDeviceSize kVramStageSize = 16u << 20;   // dirty rectangles staged per frame
static VkDeviceSize g_vramStageOff = 0;
static int g_dirtyX0 = VRAM_WIDTH, g_dirtyY0 = VRAM_HEIGHT, g_dirtyX1 = 0, g_dirtyY1 = 0;
static void markDirty(int x, int y, int w, int h)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > VRAM_WIDTH) w = VRAM_WIDTH - x;
	if (y + h > VRAM_HEIGHT) h = VRAM_HEIGHT - y;
	if (w <= 0 || h <= 0) return;
	if (x < g_dirtyX0) g_dirtyX0 = x;
	if (y < g_dirtyY0) g_dirtyY0 = y;
	if (x + w > g_dirtyX1) g_dirtyX1 = x + w;
	if (y + h > g_dirtyY1) g_dirtyY1 = y + h;
}
static bool g_vramReady = false;
static Staging g_vramStage[FRAMES];
// the frame stored into VRAM (the game's pause background, fades): blitted small, read back two frames later
static Image g_fbSmall;
static Staging g_fbRead[FRAMES];
struct FbPending { bool valid; int x, y, w, h; };
static FbPending g_fbPending[FRAMES];   // kVramUpdates copies each
static std::vector<Tex> g_tex;
static TextureID g_fmvTex = 0;
static TextureID g_fmvTexId() { return g_fmvTex; }
static VkDescriptorSet g_vramSet, g_presentSet;
static VkPipeline g_presentPipe, g_bloomPipe;
static Image g_bloom;
static bool g_bloomUsed = false;
static VkImageView g_depthSampleView;
static int g_bloomW, g_bloomH;

// vertex ring, one per frame in flight
static VkBuffer g_vtxBuf[FRAMES];
static VkDeviceMemory g_vtxMem[FRAMES];
static GrVertex* g_vtxMap[FRAMES];
static uint32_t g_vtxCursor = 0, g_vtxBase = 0;
// the frame's parameters (the game's projections and every effect's settings), one slot per change, per frame:
// shaders/glsl/driver2vulkan/fx.glsl has the same layout
struct FxUBO {
	float proj[16], proj3d[16];
	float viewToWorld[12];
	float lightVP[3][16];
	float spotVP[3][16];
	float lightA[16][4], lightB[16][4], lightC[16][4];
	float hdHole[32][4];
	float dispW, dispH, shadowSize, shadowStrength;
	float shadowBias, wet, lightStrength, fogR;
	float fogG, fogB, fogStart, fogEnd;
	int32_t lightN, fogOn, hdMask, shadowDebug;
	int32_t shadowOn;
	float skyH, farOfsX, farOfsY;
	uint32_t farCol[36];
};
static const uint32_t kFxStride = 4096;
static_assert(sizeof(FxUBO) <= kFxStride, "FxUBO slot");
static const uint32_t kUboSlots = 2048;
static VkBuffer g_uboBuf[FRAMES];
static VkDeviceMemory g_uboMem[FRAMES];
static unsigned char* g_uboMap[FRAMES];
static VkDescriptorSet g_frameSet[FRAMES];
static uint32_t g_uboSlot = 0;
static FxUBO g_fx;
static bool g_projDirty = true;   // any parameter changed: the next draw gets a new slot

// shadow maps, headlight maps, HD pages (descriptor set 2)
static Image g_shadowImg, g_spotImg, g_hdImg;
static VkSampler g_samplerShadow, g_samplerHD;
static VkDescriptorSetLayout g_fxSetLayout;
static VkDescriptorSet g_fxSet;
static const int kSpotSize = 1024;
static const VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;
static bool g_shadowReady = false;

static unsigned g_frameNo = 0;
static size_t farChunkCount();
static int g_traceDraws = -1;       // >= 0: tracing this frame's draws (armed by the file /app0/tracenow)
static double nowMs() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }
static double g_pfFrame = 0, g_pfWait = 0, g_pfSubmit = 0, g_pfLast = 0, g_pfDraws = 0, g_pfVerts = 0;
static unsigned g_pfN = 0;
extern unsigned int g_pgxpVertexIndex;
static unsigned g_pfPgxpMax = 0, g_pfVtxMax = 0, g_pfPgxpOver = 0;
static double g_pfSkipped = 0, g_pfStore = 0, g_pfOffscreen = 0, g_pfReadFb = 0;
static double g_pfFarDraws = 0, g_pfFarVerts = 0;
static double g_pfCpuAcc[4] = { 0, 0, 0, 0 };   // the game's own timers (GR_PS5_PerfCpu): logic, build+draw, DrawAllSplits, shadow pass
// ---- frame / draw state ----
static VkCommandBuffer g_cmd = VK_NULL_HANDLE;
static bool g_frameOpen = false, g_rendering = false;
static uint32_t g_slot = 0;
static VkImageLayout g_colorLayout = VK_IMAGE_LAYOUT_UNDEFINED, g_depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
static bool g_firstFrame = true;

static TextureID g_curTex = kTexVram;
static int g_texMode = 2;
static float g_texel[2] = { 1.0f / 256, 1.0f / 256 };
static int g_blend = BM_NONE;
static int g_depthTest = 0, g_stencilMode = 0, g_scissorOn = 0, g_polyOffset = 0;
static float g_polyOffsetVal = 0;
static VkViewport g_viewport;
static VkRect2D g_scissor;
static RECT16 g_clipRect;
static int g_surfaceW, g_surfaceH;
static std::vector<std::pair<uint64_t, VkPipeline>> g_pipes;

static inline void check(VkResult r, const char* what)
{
	if (r != VK_SUCCESS) { LOGF("vulkan: %s failed (%d)\n", what, (int)r); }
}

// ---------------------------------------------------------------------------------------------
// resources
// ---------------------------------------------------------------------------------------------
static void createImage(Image& img, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
	VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ci.imageType = VK_IMAGE_TYPE_2D;
	ci.format = fmt;
	ci.extent = { w, h, 1 };
	ci.mipLevels = 1;
	ci.arrayLayers = 1;
	ci.samples = VK_SAMPLE_COUNT_1_BIT;
	ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = usage;
	ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	check(vkCreateImage(H->device, &ci, nullptr, &img.image), "vkCreateImage");
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(H->device, img.image, &mr);
	VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = H->vulkanDevice->getMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	check(vkAllocateMemory(H->device, &ai, nullptr, &img.memory), "vkAllocateMemory(image)");
	check(vkBindImageMemory(H->device, img.image, img.memory, 0), "vkBindImageMemory");
	VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vi.image = img.image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = fmt;
	vi.subresourceRange = { aspect, 0, 1, 0, 1 };
	check(vkCreateImageView(H->device, &vi, nullptr, &img.view), "vkCreateImageView");
	img.format = fmt; img.w = w; img.h = h;
}

static void createImageArray(Image& img, uint32_t w, uint32_t h, uint32_t layers, uint32_t mips, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, bool perLayerViews)
{
	VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ci.imageType = VK_IMAGE_TYPE_2D;
	ci.format = fmt;
	ci.extent = { w, h, 1 };
	ci.mipLevels = mips;
	ci.arrayLayers = layers;
	ci.samples = VK_SAMPLE_COUNT_1_BIT;
	ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = usage;
	ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	check(vkCreateImage(H->device, &ci, nullptr, &img.image), "vkCreateImage(array)");
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(H->device, img.image, &mr);
	VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = H->vulkanDevice->getMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	check(vkAllocateMemory(H->device, &ai, nullptr, &img.memory), "vkAllocateMemory(array)");
	check(vkBindImageMemory(H->device, img.image, img.memory, 0), "vkBindImageMemory");
	VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vi.image = img.image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	vi.format = fmt;
	vi.subresourceRange = { aspect, 0, mips, 0, layers };
	check(vkCreateImageView(H->device, &vi, nullptr, &img.view), "vkCreateImageView(array)");
	if (perLayerViews) {
		for (uint32_t l = 0; l < layers; l++) {
			VkImageViewCreateInfo lv = vi;
			lv.viewType = VK_IMAGE_VIEW_TYPE_2D;
			lv.subresourceRange = { aspect, 0, 1, l, 1 };
			VkImageView v = VK_NULL_HANDLE;
			check(vkCreateImageView(H->device, &lv, nullptr, &v), "vkCreateImageView(layer)");
			img.layerViews.push_back(v);
		}
	}
	img.format = fmt; img.w = w; img.h = h;
}

static void createStaging(Staging& s, VkDeviceSize size)
{
	VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = size;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	check(vkCreateBuffer(H->device, &bi, nullptr, &s.buffer), "vkCreateBuffer(staging)");
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(H->device, s.buffer, &mr);
	VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = H->vulkanDevice->getMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	check(vkAllocateMemory(H->device, &ai, nullptr, &s.memory), "vkAllocateMemory(staging)");
	check(vkBindBufferMemory(H->device, s.buffer, s.memory, 0), "vkBindBufferMemory");
	check(vkMapMemory(H->device, s.memory, 0, size, 0, &s.mapped), "vkMapMemory");
	s.size = size;
}

static void createHostBuf(VkBuffer& buf, VkDeviceMemory& mem, void** mapped, VkDeviceSize size, VkBufferUsageFlags usage)
{
	VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = size;
	bi.usage = usage;
	check(vkCreateBuffer(H->device, &bi, nullptr, &buf), "vkCreateBuffer");
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(H->device, buf, &mr);
	VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = H->vulkanDevice->getMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	check(vkAllocateMemory(H->device, &ai, nullptr, &mem), "vkAllocateMemory");
	check(vkBindBufferMemory(H->device, buf, mem, 0), "vkBindBufferMemory");
	check(vkMapMemory(H->device, mem, 0, size, 0, mapped), "vkMapMemory");
}

static VkShaderModule loadModule(const char* name)
{
	std::string path = H->shadersPath + "driver2vulkan/" + name;
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) { LOGF("vulkan: shader %s missing\n", path.c_str()); return VK_NULL_HANDLE; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint32_t> code((n + 3) / 4);
	fread(code.data(), 1, n, f);
	fclose(f);
	VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	ci.codeSize = n;
	ci.pCode = code.data();
	VkShaderModule m = VK_NULL_HANDLE;
	check(vkCreateShaderModule(H->device, &ci, nullptr, &m), "vkCreateShaderModule");
	return m;
}

static void barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
	VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
	VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
	b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
	b.oldLayout = from; b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = { aspect, 0, 1, 0, 1 };
	VkDependencyInfo d{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	d.imageMemoryBarrierCount = 1;
	d.pImageMemoryBarriers = &b;
	vkCmdPipelineBarrier2(cmd, &d);
}

static void barrierRange(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
	VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
	uint32_t baseLayer, uint32_t layers, uint32_t baseLevel = 0, uint32_t levels = 1)
{
	VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
	b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
	b.oldLayout = from; b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = { aspect, baseLevel, levels, baseLayer, layers };
	VkDependencyInfo d{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	d.imageMemoryBarrierCount = 1;
	d.pImageMemoryBarriers = &b;
	vkCmdPipelineBarrier2(cmd, &d);
}

static VkDescriptorSet makeTexSet(VkImageView view, VkSampler sampler = VK_NULL_HANDLE)
{
	VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	ai.descriptorPool = g_descPool;
	ai.descriptorSetCount = 1;
	ai.pSetLayouts = &g_texSetLayout;
	VkDescriptorSet set = VK_NULL_HANDLE;
	check(vkAllocateDescriptorSets(H->device, &ai, &set), "vkAllocateDescriptorSets");
	VkDescriptorImageInfo info[4] = {
		{ sampler ? sampler : g_samplerNearest, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ g_samplerNearest, g_lut.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ g_samplerNearest, g_farPageImg.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ g_samplerNearest, g_farPalImg.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
	};
	VkWriteDescriptorSet w[4]{};
	for (int i = 0; i < 4; i++) {
		w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[i].dstSet = set;
		w[i].dstBinding = i;
		w[i].descriptorCount = 1;
		w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w[i].pImageInfo = &info[i];
	}
	vkUpdateDescriptorSets(H->device, 4, w, 0, nullptr);
	return set;
}

// A one-time command buffer for uploads outside the frame
static VkCommandBuffer beginOneTime()
{
	return H->vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
}
static void endOneTime(VkCommandBuffer cmd)
{
	H->vulkanDevice->flushCommandBuffer(cmd, H->queue, true);
}

static void uploadImage(Image& img, const void* data, size_t size, VkImageLayout finalLayout)
{
	Staging st;
	createStaging(st, size);
	memcpy(st.mapped, data, size);
	VkCommandBuffer cmd = beginOneTime();
	barrier(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	VkBufferImageCopy c{};
	c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	c.imageExtent = { img.w, img.h, 1 };
	vkCmdCopyBufferToImage(cmd, st.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
	barrier(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, finalLayout,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	endOneTime(cmd);
	vkDestroyBuffer(H->device, st.buffer, nullptr);
	vkFreeMemory(H->device, st.memory, nullptr);
}

// ---------------------------------------------------------------------------------------------
// pipelines
// ---------------------------------------------------------------------------------------------
// kind: 0 the scene, 1 a shadow map (depth only), 2 the depth prepass (the scene's targets, no colour written)
// depthMode: 0 off, 1 test and write (LEQUAL), 2 equal and no write (the colour draws after a prepass)
static VkPipeline buildPipeline(int kind, int blend, int depthMode, int stencil, int polyOffset)
{
	VkVertexInputBindingDescription bind{ 0, sizeof(GrVertex), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attr[5] = {
		{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 },
		{ 2, 0, VK_FORMAT_R8G8B8A8_USCALED, 32 },
		{ 3, 0, VK_FORMAT_R8G8B8A8_UNORM, 36 },
		{ 4, 0, VK_FORMAT_R8G8B8A8_SSCALED, 40 },
	};
	VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
	vi.vertexAttributeDescriptionCount = 5; vi.pVertexAttributeDescriptions = attr;
	VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vp.viewportCount = 1; vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;
	rs.depthBiasEnable = (polyOffset || kind == 1) ? VK_TRUE : VK_FALSE;
	VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	ds.depthTestEnable = depthMode ? VK_TRUE : VK_FALSE;
	ds.depthWriteEnable = (depthMode == 1) ? VK_TRUE : VK_FALSE;
	ds.depthCompareOp = (depthMode == 2) ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
	if (kind != 1) {
		ds.stencilTestEnable = VK_TRUE;
		if (stencil) {
			ds.front.compareOp = VK_COMPARE_OP_ALWAYS;
			ds.front.failOp = ds.front.passOp = ds.front.depthFailOp = VK_STENCIL_OP_REPLACE;
			ds.front.compareMask = 0x10;
		} else {
			ds.front.compareOp = VK_COMPARE_OP_NOT_EQUAL;
			ds.front.failOp = VK_STENCIL_OP_REPLACE;
			ds.front.passOp = VK_STENCIL_OP_KEEP;
			ds.front.depthFailOp = VK_STENCIL_OP_KEEP;
			ds.front.compareMask = 0xFF;
		}
		ds.front.writeMask = 0xFF;
		ds.front.reference = 1;
		ds.back = ds.front;
	}
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = (kind == 2) ? 0 : 0xF;
	if (blend != BM_NONE && kind == 0) {
		cba.blendEnable = VK_TRUE;
		cba.colorBlendOp = (blend == BM_SUBTRACT) ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
		cba.alphaBlendOp = VK_BLEND_OP_ADD;
		VkBlendFactor src = VK_BLEND_FACTOR_ONE, dst = VK_BLEND_FACTOR_ONE;
		if (blend == BM_AVERAGE) { src = VK_BLEND_FACTOR_SRC_ALPHA; dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; }
		else if (blend == BM_ADD_QUATER_SOURCE) { src = VK_BLEND_FACTOR_CONSTANT_ALPHA; dst = VK_BLEND_FACTOR_ONE; }
		cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = src;
		cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = dst;
	}
	VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cb.attachmentCount = (kind == 1) ? 0 : 1; cb.pAttachments = &cba;
	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
	VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dy.dynamicStateCount = 4; dy.pDynamicStates = dyn;
	VkPipelineShaderStageCreateInfo st[2]{};
	st[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, g_vs, "main" };
	st[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, g_fs, "main" };
	VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	if (kind == 1) {
		ri.colorAttachmentCount = 0;
		ri.depthAttachmentFormat = kShadowFormat;
	} else {
		ri.colorAttachmentCount = 1;
		ri.pColorAttachmentFormats = &g_color.format;
		ri.depthAttachmentFormat = g_depth.format;
		ri.stencilAttachmentFormat = g_depth.format;
	}
	VkGraphicsPipelineCreateInfo pi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pi.pNext = &ri;
	pi.stageCount = 2; pi.pStages = st;
	pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
	pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds;
	pi.pColorBlendState = &cb; pi.pDynamicState = &dy;
	pi.layout = g_pipeLayout;
	VkPipeline p = VK_NULL_HANDLE;
	check(vkCreateGraphicsPipelines(H->device, H->pipelineCache, 1, &pi, nullptr, &p), "vkCreateGraphicsPipelines");
	return p;
}

// what the draws do now (set by the GR_* calls)
static bool g_inShadow = false;          // drawing into a shadow map
static int g_prepassKind = 0;            // 2 while the depth prepass runs
static bool g_prepassColour = false;     // the colour draws after a prepass: depth equal
static int g_pass = 0;                   // the shader's pass (push constant), for the scene draws
static int g_passNow = 0;                // 2 while the frame's shadows are valid
static int g_cascade = 0, g_cutout = 0;
static int g_shadowSize = 0;             // size of the map being drawn

static int currentKind() { return g_inShadow ? 1 : (g_prepassKind ? 2 : 0); }

static VkPipeline getPipeline()
{
	const int kind = currentKind();
	int depthMode = 0;
	if (kind == 1 || kind == 2) depthMode = 1;
	else if (g_depthTest && g_cfg_pgxpZBuffer) depthMode = g_prepassColour ? 2 : 1;
	const int blend = (kind == 0) ? g_blend : BM_NONE;
	const int stencil = (kind == 1) ? 0 : g_stencilMode;
	const int poly = (kind == 0) ? g_polyOffset : 0;
	const uint64_t key = (uint64_t)blend | ((uint64_t)depthMode << 3) | ((uint64_t)stencil << 5) | ((uint64_t)poly << 6) | ((uint64_t)kind << 7);
	for (auto& e : g_pipes)
		if (e.first == key) return e.second;
	VkPipeline p = buildPipeline(kind, blend, depthMode, stencil, poly);
	g_pipes.push_back({ key, p });
	return p;
}

// ---------------------------------------------------------------------------------------------
// frame control
// ---------------------------------------------------------------------------------------------
static void beginRendering(bool clear)
{
	if (g_rendering) return;
	barrier(g_cmd, g_color.image, VK_IMAGE_ASPECT_COLOR_BIT, g_colorLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
	barrier(g_cmd, g_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, g_depthLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
		VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
	g_colorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	g_depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	VkRenderingAttachmentInfo ca{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	ca.imageView = g_color.view;
	ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	ca.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
	ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	ca.clearValue.color = { { 0, 0, 0, 1 } };
	VkRenderingAttachmentInfo da{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	da.imageView = g_depth.view;
	da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	da.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
	da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	da.clearValue.depthStencil = { 1.0f, 0 };
	VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
	ri.renderArea = { { 0, 0 }, { g_color.w, g_color.h } };
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &ca;
	ri.pDepthAttachment = &da;
	ri.pStencilAttachment = &da;
	vkCmdBeginRendering(g_cmd, &ri);
	g_rendering = true;
}

// a shadow map layer being drawn (1) or none (0)
static int g_depthTarget = 0;
static Image* g_depthTargetImg = nullptr;
static int g_depthTargetLayer = 0;

static void endRendering()
{
	if (!g_rendering) return;
	vkCmdEndRendering(g_cmd);
	g_rendering = false;
	if (g_depthTarget) {
		// the finished map is read by the scene's shaders
		barrierRange(g_cmd, g_depthTargetImg->image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, g_depthTargetLayer, 1);
		g_depthTarget = 0;
	}
}

// begins drawing into one layer of a shadow map array, cleared
static void beginDepthRendering(Image& arr, int layer, int size)
{
	endRendering();
	barrierRange(g_cmd, arr.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
		VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, layer, 1);
	VkRenderingAttachmentInfo da{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	da.imageView = arr.layerViews[layer];
	da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
	da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	da.clearValue.depthStencil = { 1.0f, 0 };
	VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
	ri.renderArea = { { 0, 0 }, { (uint32_t)size, (uint32_t)size } };
	ri.layerCount = 1;
	ri.pDepthAttachment = &da;
	vkCmdBeginRendering(g_cmd, &ri);
	g_rendering = true;
	g_depthTarget = 1;
	g_depthTargetImg = &arr;
	g_depthTargetLayer = layer;
	g_shadowSize = size;
}

// The game wrote to VRAM: a new copy of it, in the command stream where the game asked, so the draws
// before it keep the old picture and the draws after it see the new one (as OpenGL's queue did).
static void uploadVram()
{
	endRendering();
	int x0 = g_dirtyX0, y0 = g_dirtyY0, x1 = g_dirtyX1, y1 = g_dirtyY1;
	if (!g_vramReady || x1 <= x0 || y1 <= y0) { x0 = 0; y0 = 0; x1 = VRAM_WIDTH; y1 = VRAM_HEIGHT; }
	const int w = x1 - x0, h = y1 - y0;
	const VkDeviceSize bytes = (VkDeviceSize)w * h * 2;
	g_vramStageOff = (g_vramStageOff + 15) & ~(VkDeviceSize)15;
	if (g_vramStageOff + bytes > kVramStageSize) {
		LOGF("vulkan: VRAM staging full, an upload skipped\n");
		return;
	}
	unsigned char* dst = (unsigned char*)g_vramStage[g_slot].mapped + g_vramStageOff;
	for (int y = 0; y < h; y++)
		memcpy(dst + (size_t)y * w * 2, vram + (size_t)(y0 + y) * VRAM_WIDTH + x0, (size_t)w * 2);
	// one image, changed in place: the queue runs the draws before this copy first, and the draws after it see it
	barrier(g_cmd, g_vramImg.image, VK_IMAGE_ASPECT_COLOR_BIT, g_vramReady ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	VkBufferImageCopy c{};
	c.bufferOffset = g_vramStageOff;
	c.bufferRowLength = w;
	c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	c.imageOffset = { x0, y0, 0 };
	c.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
	vkCmdCopyBufferToImage(g_cmd, g_vramStage[g_slot].buffer, g_vramImg.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
	barrier(g_cmd, g_vramImg.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	g_vramStageOff += bytes;
	g_dirtyX0 = VRAM_WIDTH; g_dirtyY0 = VRAM_HEIGHT; g_dirtyX1 = 0; g_dirtyY1 = 0;
	vram_need_update = 0;
	g_vramReady = true;
}

static void openFrame()
{
	if (g_frameOpen) return;
	const double w0 = nowMs();
	g_cmd = H->beginFrame();
	g_pfWait += nowMs() - w0;
	g_slot = H->frameSlot();
	g_frameOpen = true;
	g_vtxCursor = 0;
	g_vtxBase = 0;
	g_uboSlot = 0;
	g_projDirty = true;
	g_vramStageOff = 0;
	if (g_fbPending[g_slot].valid) {
		const FbPending& fp = g_fbPending[g_slot];
		const unsigned char* src = (const unsigned char*)g_fbRead[g_slot].mapped;
		for (int y = 0; y < fp.h; y++) {
			unsigned short* dst = vram + (size_t)(fp.y + y) * VRAM_WIDTH + fp.x;
			const unsigned char* row = src + (size_t)y * fp.w * 4;
			for (int x = 0; x < fp.w; x++, row += 4)
				dst[x] = (unsigned short)((row[0] >> 3) | ((row[1] >> 3) << 5) | ((row[2] >> 3) << 10) | 0x8000);
		}
		markDirty(fp.x, fp.y, fp.w, fp.h);
		vram_need_update = 1;
		g_fbPending[g_slot].valid = false;
	}
	if (!g_vramReady || vram_need_update) uploadVram();
}

static void ensureRendering()
{
	openFrame();
	if (!g_rendering && !g_inShadow) beginRendering(g_firstFrame);
	if (!g_inShadow) g_firstFrame = false;
}

static void flushState()
{
	VkPipeline p = getPipeline();
	vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
	if (g_inShadow) {
		// shadow maps are sampled with the light's clip coordinates as they are: no flipped viewport
		VkViewport vp{ 0, 0, (float)g_shadowSize, (float)g_shadowSize, 0.0f, 1.0f };
		VkRect2D sc{ { 0, 0 }, { (uint32_t)g_shadowSize, (uint32_t)g_shadowSize } };
		vkCmdSetViewport(g_cmd, 0, 1, &vp);
		vkCmdSetScissor(g_cmd, 0, 1, &sc);
		vkCmdSetDepthBias(g_cmd, 4.0f, 0.0f, 2.0f);
	} else {
		vkCmdSetViewport(g_cmd, 0, 1, &g_viewport);
		VkRect2D sc = g_scissorOn ? g_scissor : VkRect2D{ { 0, 0 }, { g_color.w, g_color.h } };
		vkCmdSetScissor(g_cmd, 0, 1, &sc);
		vkCmdSetDepthBias(g_cmd, g_polyOffsetVal, 0.0f, 0.0f);
	}
	const float bc[4] = { 0.25f, 0.25f, 0.25f, 0.5f };
	vkCmdSetBlendConstants(g_cmd, bc);
}

// ---------------------------------------------------------------------------------------------
// the GR_* interface
// ---------------------------------------------------------------------------------------------
extern "C" void red2_attach(Red2Host* host) { H = host; }
static bool g_skipDraws = false;   // while the game draws into a VRAM picture (not supported yet)

static void identity(float* m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }

int GR_InitialiseRender(char*, int width, int height, int)
{
	g_windowWidth = width;
	g_windowHeight = height;
	readFrameInterval();
	return H ? 1 : 0;
}

static VkPipeline buildPresentPipeline(VkFormat colorFormat, VkFormat depthFormat)
{
	VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vp.viewportCount = 1; vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cb.attachmentCount = 1; cb.pAttachments = &cba;
	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
	VkPipelineShaderStageCreateInfo st[2]{};
	st[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, g_pvs, "main" };
	st[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, g_pfs, "main" };
	VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &colorFormat;
	ri.depthAttachmentFormat = depthFormat; ri.stencilAttachmentFormat = depthFormat;
	VkGraphicsPipelineCreateInfo pi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pi.pNext = &ri; pi.stageCount = 2; pi.pStages = st;
	pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
	pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds;
	pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = g_presentLayout;
	VkPipeline p = VK_NULL_HANDLE;
	check(vkCreateGraphicsPipelines(H->device, H->pipelineCache, 1, &pi, nullptr, &p), "present pipeline");
	return p;
}

static void initEffects();

int GR_InitialisePSX()
{
	g_surfaceW = g_windowWidth;
	g_surfaceH = g_windowHeight;
	if (g_cfg_ps5RenderScale < 1) g_cfg_ps5RenderScale = 1;
	if (g_cfg_ps5RenderScale > 2) g_cfg_ps5RenderScale = 2;
	g_windowWidth *= g_cfg_ps5RenderScale;
	g_windowHeight *= g_cfg_ps5RenderScale;
	LOGF("vulkan: render target %dx%d, surface %ux%u\n", g_windowWidth, g_windowHeight, H->surfaceW, H->surfaceH);

	memset(vram, 0, sizeof(vram));
	for (int y = 0; y < LUT_HEIGHT; y++)
		for (int x = 0; x < LUT_WIDTH; x++) {
			const unsigned c = (y << 8) | x;
			unsigned char* px = rgLUT + (y * LUT_WIDTH + x) * 4;
			px[0] = (unsigned char)((c & 31) << 3);
			px[1] = (unsigned char)(((c >> 5) & 31) << 3);
			px[2] = (unsigned char)(((c >> 10) & 31) << 3);
			px[3] = (unsigned char)(((c >> 15) & 1) << 7);
		}

	VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	si.magFilter = si.minFilter = VK_FILTER_NEAREST;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.maxLod = 0.25f;
	check(vkCreateSampler(H->device, &si, nullptr, &g_samplerNearest), "vkCreateSampler");
	si.magFilter = si.minFilter = VK_FILTER_LINEAR;
	check(vkCreateSampler(H->device, &si, nullptr, &g_samplerLinear), "vkCreateSampler");
	// VRAM wraps: tpage values with mode bits make the shader's page row run past 512, and OpenGL's default wrapped it
	si.magFilter = si.minFilter = VK_FILTER_NEAREST;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	check(vkCreateSampler(H->device, &si, nullptr, &g_samplerRepeat), "vkCreateSampler");

	// descriptors
	VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 16 } };
	VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	dpi.maxSets = 512; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
	dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	check(vkCreateDescriptorPool(H->device, &dpi, nullptr, &g_descPool), "vkCreateDescriptorPool");
	VkDescriptorSetLayoutBinding tb[4]{};
	for (int i = 0; i < 4; i++) {
		tb[i].binding = i; tb[i].descriptorCount = 1;
		tb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		tb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	lci.bindingCount = 4; lci.pBindings = tb;
	check(vkCreateDescriptorSetLayout(H->device, &lci, nullptr, &g_texSetLayout), "texSetLayout");
	VkDescriptorSetLayoutBinding fb{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
	lci.bindingCount = 1; lci.pBindings = &fb;
	check(vkCreateDescriptorSetLayout(H->device, &lci, nullptr, &g_frameSetLayout), "frameSetLayout");
	VkDescriptorSetLayoutBinding pb[3]{};
	for (int i = 0; i < 3; i++) {
		pb[i].binding = i; pb[i].descriptorCount = 1;
		pb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		pb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	lci.bindingCount = 3; lci.pBindings = pb;
	check(vkCreateDescriptorSetLayout(H->device, &lci, nullptr, &g_presentSetLayout), "presentSetLayout");

	VkDescriptorSetLayoutBinding eb[3]{};
	for (int i = 0; i < 3; i++) {
		eb[i].binding = i; eb[i].descriptorCount = 1;
		eb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		eb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	lci.bindingCount = 3; lci.pBindings = eb;
	check(vkCreateDescriptorSetLayout(H->device, &lci, nullptr, &g_fxSetLayout), "fxSetLayout");

	VkDescriptorSetLayout sets[3] = { g_texSetLayout, g_frameSetLayout, g_fxSetLayout };
	VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
	VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pli.setLayoutCount = 3; pli.pSetLayouts = sets; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
	check(vkCreatePipelineLayout(H->device, &pli, nullptr, &g_pipeLayout), "pipelineLayout");
	VkPushConstantRange ppc{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
	pli.setLayoutCount = 1; pli.pSetLayouts = &g_presentSetLayout; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &ppc;
	check(vkCreatePipelineLayout(H->device, &pli, nullptr, &g_presentLayout), "presentLayout");

	g_vs = loadModule("psx.vert.spv");
	g_fs = loadModule("psx.frag.spv");
	g_pvs = loadModule("present.vert.spv");
	g_pfs = loadModule("present.frag.spv");

	// targets
	createImage(g_color, g_windowWidth, g_windowHeight, VK_FORMAT_R8G8B8A8_UNORM,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
	createImage(g_depth, g_windowWidth, g_windowHeight, H->depthFormat,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);

	// the colour lookup
	createImage(g_lut, LUT_WIDTH, LUT_HEIGHT, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
	uploadImage(g_lut, rgLUT, sizeof(rgLUT), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	// the far field's page store
	createImageArray(g_farPageImg, 256, 256, kFarPages, 1, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, false);
	createImage(g_farPalImg, 16, kFarPages * kFarPalRows, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
	g_farPageHave.assign(kFarPages, 0);
	{
		VkCommandBuffer cmd0 = beginOneTime();
		barrierRange(cmd0, g_farPageImg.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, kFarPages);
		barrier(cmd0, g_farPalImg.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		endOneTime(cmd0);
	}

	// VRAM images, and the white texture (id 2)
	createImage(g_fbSmall, VRAM_WIDTH, VRAM_HEIGHT, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
	for (int i = 0; i < (int)FRAMES; i++) createStaging(g_fbRead[i], (VkDeviceSize)VRAM_WIDTH * VRAM_HEIGHT * 4);
	createImage(g_vramImg, VRAM_WIDTH, VRAM_HEIGHT, VK_FORMAT_R8G8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
	g_vramSet = makeTexSet(g_vramImg.view, g_samplerRepeat);
	for (int i = 0; i < (int)FRAMES; i++)
		createStaging(g_vramStage[i], kVramStageSize);
	g_tex.resize(3);
	g_tex[kTexVram].alive = true;
	{
		Tex& t = g_tex[kTexWhite];
		t.alive = true;
		createImage(t.img, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
		const unsigned white = 0xFFFFFFFFu;
		uploadImage(t.img, &white, 4, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		t.set = makeTexSet(t.img.view);
	}

	// vertex rings and matrix slots
	for (int i = 0; i < (int)FRAMES; i++) {
		void* m;
		createHostBuf(g_vtxBuf[i], g_vtxMem[i], &m, sizeof(GrVertex) * (VkDeviceSize)MAX_VERTEX_BUFFER_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
		g_vtxMap[i] = (GrVertex*)m;
		createHostBuf(g_uboBuf[i], g_uboMem[i], &m, (VkDeviceSize)kFxStride * kUboSlots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
		g_uboMap[i] = (unsigned char*)m;
		VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		ai.descriptorPool = g_descPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &g_frameSetLayout;
		check(vkAllocateDescriptorSets(H->device, &ai, &g_frameSet[i]), "frame set");
		VkDescriptorBufferInfo bi{ g_uboBuf[i], 0, sizeof(FxUBO) };
		VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		w.dstSet = g_frameSet[i]; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w.pBufferInfo = &bi;
		vkUpdateDescriptorSets(H->device, 1, &w, 0, nullptr);
	}
	memset(&g_fx, 0, sizeof(g_fx));
	identity(g_fx.proj); identity(g_fx.proj3d);
	for (int i = 0; i < 32; i++) { g_fx.hdHole[i][0] = g_fx.hdHole[i][1] = 1e9f; g_fx.hdHole[i][2] = g_fx.hdHole[i][3] = -1e9f; }
	g_fx.dispW = 320; g_fx.dispH = 240;

	// the present pass: a filtered full screen triangle onto the swapchain, with the bloom and ambient occlusion
	{
		// the depth buffer is read by the ambient occlusion: a view of its depth aspect
		VkImageViewCreateInfo dv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		dv.image = g_depth.image; dv.viewType = VK_IMAGE_VIEW_TYPE_2D; dv.format = g_depth.format;
		dv.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
		check(vkCreateImageView(H->device, &dv, nullptr, &g_depthSampleView), "depth sample view");
		g_bloomW = g_windowWidth / 8; g_bloomH = g_windowHeight / 8;
		createImage(g_bloom, g_bloomW, g_bloomH, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

		VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		ai.descriptorPool = g_descPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &g_presentSetLayout;
		check(vkAllocateDescriptorSets(H->device, &ai, &g_presentSet), "present set");
		VkDescriptorImageInfo ii[3] = {
			{ g_samplerLinear, g_color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ g_samplerNearest, g_depthSampleView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
			{ g_samplerLinear, g_bloom.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		};
		VkWriteDescriptorSet w[3]{};
		for (int i = 0; i < 3; i++) {
			w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			w[i].dstSet = g_presentSet; w[i].dstBinding = i; w[i].descriptorCount = 1;
			w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[i].pImageInfo = &ii[i];
		}
		vkUpdateDescriptorSets(H->device, 3, w, 0, nullptr);
		// the base class's present pass has the swapchain format and its depth/stencil buffer; the bloom target is colour only
		g_presentPipe = buildPresentPipeline(H->swapFormat, H->depthFormat);
		g_bloomPipe = buildPresentPipeline(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED);
	}
	g_viewport = { 0, (float)g_windowHeight, (float)g_windowWidth, -(float)g_windowHeight, 0.0f, 1.0f };
	g_scissor = { { 0, 0 }, { (uint32_t)g_windowWidth, (uint32_t)g_windowHeight } };
	initEffects();
	LOGF("vulkan: renderer ready\n");
	return 1;
}

void GR_Shutdown() {}
void GR_UpdateSwapIntervalState(int) {}
void GR_ResetDevice() {}

void GR_BeginScene()
{
	ensureRendering();
	g_curTex = 0;
	// a new scene starts with a clean depth and stencil
	VkClearAttachment ca{ VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, {} };
	ca.clearValue.depthStencil = { 1.0f, 0 };
	VkClearRect cr{ { { 0, 0 }, { g_color.w, g_color.h } }, 0, 1 };
	vkCmdClearAttachments(g_cmd, 1, &ca, 1, &cr);
	g_viewport = { 0, (float)g_windowHeight, (float)g_windowWidth, -(float)g_windowHeight, 0.0f, 1.0f };
}

void GR_EndScene() { framebuffer_need_update = 1; }


// ---- debugging: the game's picture and its VRAM as PPM files in the title folder ----
static VkBuffer g_dumpBuf = VK_NULL_HANDLE;
static VkDeviceMemory g_dumpMem = VK_NULL_HANDLE;
static void* g_dumpMap = nullptr;
// dumped when the file /app0/dumpnow exists (uploaded by FTP while the game runs); it is removed after
static bool dumpWanted(unsigned frame)
{
	if (frame % 20) return false;
	FILE* f = fopen("/app0/dumpnow", "rb");
	if (!f) return false;
	fclose(f);
	remove("/app0/dumpnow");
	return true;
}
static void dumpVramPpm(unsigned frame)
{
	char name[96];
	snprintf(name, sizeof(name), "/app0/dump_vram_%u.ppm", frame);
	FILE* f = fopen(name, "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", VRAM_WIDTH, VRAM_HEIGHT);
	std::vector<unsigned char> row(VRAM_WIDTH * 3);
	for (int y = 0; y < VRAM_HEIGHT; y++) {
		for (int x = 0; x < VRAM_WIDTH; x++) {
			const unsigned c = vram[y * VRAM_WIDTH + x];
			row[x * 3 + 0] = (unsigned char)((c & 31) << 3);
			row[x * 3 + 1] = (unsigned char)(((c >> 5) & 31) << 3);
			row[x * 3 + 2] = (unsigned char)(((c >> 10) & 31) << 3);
		}
		fwrite(row.data(), 1, row.size(), f);
	}
	fclose(f);
}
static void dumpColourPpm(unsigned frame)
{
	char name[96];
	snprintf(name, sizeof(name), "/app0/dump_frame_%u.ppm", frame);
	FILE* f = fopen(name, "wb");
	if (!f) return;
	fprintf(f, "P6\n%u %u\n255\n", g_color.w, g_color.h);
	const unsigned char* src = (const unsigned char*)g_dumpMap;
	std::vector<unsigned char> row(g_color.w * 3);
	for (uint32_t y = 0; y < g_color.h; y++) {
		for (uint32_t x = 0; x < g_color.w; x++) {
			row[x * 3 + 0] = src[(y * g_color.w + x) * 4 + 0];
			row[x * 3 + 1] = src[(y * g_color.w + x) * 4 + 1];
			row[x * 3 + 2] = src[(y * g_color.w + x) * 4 + 2];
		}
		fwrite(row.data(), 1, row.size(), f);
	}
	fclose(f);
}

extern "C" {

void GR_SwapWindow()
{
	openFrame();
	endRendering();
	// the game's picture and its depth become textures for the present pass
	barrier(g_cmd, g_color.image, VK_IMAGE_ASPECT_COLOR_BIT, g_colorLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	g_colorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier(g_cmd, g_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, g_depthLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	g_depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	struct { float bloom, thr, ao; int mode; float pixel[2]; float bloomPixel[2]; } ppc = {
		g_ps5ModOn ? g_cfg_ps5Bloom : 0.0f, 0.8f, g_ps5ModOn ? g_cfg_ps5SSAO : 0.0f, 0,
		{ 1.0f / (float)g_windowWidth, 1.0f / (float)g_windowHeight }, { 1.0f / (float)g_bloomW, 1.0f / (float)g_bloomH } };
	if (ppc.bloom > 0.0f) {
		// the bright parts of the picture averaged into a small texture (the glow is read from it below)
		barrier(g_cmd, g_bloom.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
		VkRenderingAttachmentInfo ca{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		ca.imageView = g_bloom.view;
		ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
		ri.renderArea = { { 0, 0 }, { (uint32_t)g_bloomW, (uint32_t)g_bloomH } };
		ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
		vkCmdBeginRendering(g_cmd, &ri);
		VkViewport bvp{ 0, 0, (float)g_bloomW, (float)g_bloomH, 0.0f, 1.0f };
		VkRect2D bsc{ { 0, 0 }, { (uint32_t)g_bloomW, (uint32_t)g_bloomH } };
		vkCmdSetViewport(g_cmd, 0, 1, &bvp);
		vkCmdSetScissor(g_cmd, 0, 1, &bsc);
		vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_bloomPipe);
		vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_presentLayout, 0, 1, &g_presentSet, 0, nullptr);
		ppc.mode = 1;
		vkCmdPushConstants(g_cmd, g_presentLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ppc), &ppc);
		vkCmdDraw(g_cmd, 3, 1, 0, 0);
		vkCmdEndRendering(g_cmd);
		ppc.mode = 0;
		barrier(g_cmd, g_bloom.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	} else if (!g_bloomUsed) {
		// the bloom texture is bound whether it is used or not: give it a defined layout
		barrier(g_cmd, g_bloom.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	}
	g_bloomUsed = true;
	const bool dumping = dumpWanted(g_frameNo);
	if (dumping) {
		if (!g_dumpBuf) createHostBuf(g_dumpBuf, g_dumpMem, &g_dumpMap, (VkDeviceSize)g_color.w * g_color.h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
		barrier(g_cmd, g_color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		VkBufferImageCopy c{};
		c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		c.imageExtent = { g_color.w, g_color.h, 1 };
		vkCmdCopyImageToBuffer(g_cmd, g_color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_dumpBuf, 1, &c);
		barrier(g_cmd, g_color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	}
	H->beginPresentPass(g_cmd);
	VkViewport vp{ 0, 0, (float)H->surfaceW, (float)H->surfaceH, 0.0f, 1.0f };
	VkRect2D sc{ { 0, 0 }, { H->surfaceW, H->surfaceH } };
	vkCmdSetViewport(g_cmd, 0, 1, &vp);
	vkCmdSetScissor(g_cmd, 0, 1, &sc);
	vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_presentPipe);
	vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_presentLayout, 0, 1, &g_presentSet, 0, nullptr);
	vkCmdPushConstants(g_cmd, g_presentLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ppc), &ppc);
	vkCmdDraw(g_cmd, 3, 1, 0, 0);
	H->endPresentPass(g_cmd);
	const double s0 = nowMs();
	H->endFrame();
	g_pfSubmit += nowMs() - s0;
	g_frameOpen = false;
	if (dumping) {
		vkQueueWaitIdle(H->queue);
		dumpColourPpm(g_frameNo);
		dumpVramPpm(g_frameNo);
		LOGF("vulkan: dumped frame %u\n", g_frameNo);
	}
	if (g_traceDraws >= 0) { LOGF("trace: frame ends after %d draws\n", g_traceDraws); g_traceDraws = -1; }
	else if (g_frameNo % 20 == 7) {
		FILE* tf = fopen("/app0/tracenow", "rb");
		if (tf) { fclose(tf); remove("/app0/tracenow"); g_traceDraws = 0; LOGF("trace: next frame (%u)\n", g_frameNo + 1); }
	}
	g_frameNo++;
	{
		const double now = nowMs();
		if (g_pfLast > 0) g_pfFrame += now - g_pfLast;
		g_pfLast = now;
		if (++g_pfN == 120) {
			LOGF("vulkan perf: %.1f fps, frame %.2f ms (waiting for the GPU %.2f ms, present+submit %.2f ms), %.0f draws, %.0fk vertices per frame; skipped offscreen triangles %.0f, offscreen passes %.0f, StoreFrameBuffer %.0f, ReadFramebuffer %.0f (per 120 frames)\n",
				1000.0 * 120 / g_pfFrame, g_pfFrame / 120, g_pfWait / 120, g_pfSubmit / 120, g_pfDraws / 120, g_pfVerts / 120000.0, g_pfSkipped, g_pfOffscreen, g_pfStore, g_pfReadFb);
			LOGF("vulkan limits: PGXP cache max %u of %u (near the end %u times), vertex buffer max %u of %u\n", g_pfPgxpMax, 1u << 21, g_pfPgxpOver, g_pfVtxMax, (unsigned)MAX_VERTEX_BUFFER_SIZE);
			g_pfPgxpMax = g_pfVtxMax = g_pfPgxpOver = 0;
			if (g_pfFarDraws > 0) LOGF("vulkan far field: %.0f draws, %.0fk vertices per frame, %zu chunk buffers\n", g_pfFarDraws / 120, g_pfFarVerts / 120000.0, farChunkCount());
			g_pfFarDraws = g_pfFarVerts = 0;
			LOGF("vulkan cpu: logic %.2f ms, build+draw %.2f ms (DrawAllSplits %.2f, of which shadow pass %.2f)\n",
				g_pfCpuAcc[0] / 120, g_pfCpuAcc[1] / 120, g_pfCpuAcc[2] / 120, g_pfCpuAcc[3] / 120);
			for (int i = 0; i < 4; i++) g_pfCpuAcc[i] = 0;
			g_pfSkipped = g_pfStore = g_pfOffscreen = g_pfReadFb = 0;
			g_pfN = 0; g_pfFrame = g_pfWait = g_pfSubmit = g_pfDraws = g_pfVerts = 0;
		}
	}
	g_cmd = VK_NULL_HANDLE;
}

void GR_SaveVRAM(const char*, int, int, int, int, int) {}

void GR_Ortho2D(float left, float right, float bottom, float top, float znear, float zfar)
{
	const float a = 2.0f / (right - left);
	const float b = 2.0f / (top - bottom);
	const float c = 2.0f / (znear - zfar);
	const float x = (left + right) / (left - right);
	const float y = (bottom + top) / (bottom - top);
	const float z = (znear + zfar) / (znear - zfar);
	const float m[16] = { a, 0, 0, 0, 0, b, 0, 0, 0, 0, c, 0, x, y, z, 1 };
	memcpy(g_fx.proj, m, sizeof(m));
	g_projDirty = true;
}

void GR_Perspective3D(const float fov, const float width, const float height, const float zNear, const float zFar)
{
	const float sinF = sinf(0.5f * fov), cosF = cosf(0.5f * fov);
	const float h = cosF / sinF;
	const float w = (h * height) / width;
	const float m[16] = { w, 0, 0, 0, 0, h, 0, 0, 0, 0, (zFar + zNear) / (zFar - zNear), 1, 0, 0, -(2 * zFar * zNear) / (zFar - zNear), 0 };
	memcpy(g_fx.proj3d, m, sizeof(m));
	g_projDirty = true;
}

ShaderID GR_Shader_Compile(const char*, int) { return 1; }
void GR_SetShader(const ShaderID) {}

void GR_SetTexture(TextureID texture, TexFormat texFormat)
{
	g_texMode = ((int)texFormat < 0) ? 4 : (int)texFormat;
	if (g_dbg_texturelessMode) texture = kTexWhite;
	g_curTex = texture;
}

void GR_SetOverrideTextureSize(int width, int height)
{
	g_texel[0] = 1.0f / (float)width;
	g_texel[1] = 1.0f / (float)height;
}

TextureID GR_CreateRGBATexture(int width, int height, u_char* data)
{
	TextureID id = 0;
	for (size_t i = 3; i < g_tex.size(); i++)
		if (!g_tex[i].alive) { id = (TextureID)i; break; }
	if (!id) { g_tex.emplace_back(); id = (TextureID)(g_tex.size() - 1); }
	Tex& t = g_tex[id];
	t.alive = true;
	createImage(t.img, width, height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
	if (data) {
		uploadImage(t.img, data, (size_t)width * height * 4, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	} else {
		std::vector<unsigned char> zero((size_t)width * height * 4, 0);
		uploadImage(t.img, zero.data(), zero.size(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	t.set = makeTexSet(t.img.view);
	return id;
}

void GR_DestroyTexture(TextureID texture)
{
	if (texture < 3 || texture >= g_tex.size() || !g_tex[texture].alive) return;
	if (texture == g_fmvTexId()) return;   // the FMV texture lives on between videos
	// the GPU may still be reading it from a frame in flight
	vkDeviceWaitIdle(H->device);
	Tex& t = g_tex[texture];
	vkFreeDescriptorSets(H->device, g_descPool, 1, &t.set);
	vkDestroyImageView(H->device, t.img.view, nullptr);
	vkDestroyImage(H->device, t.img.image, nullptr);
	vkFreeMemory(H->device, t.img.memory, nullptr);
	t = Tex();
}

void GR_SetBlendMode(BlendMode blendMode)
{
	if (blendMode == BM_NONE) {
		g_blend = BM_NONE;
		g_depthTest = 1;
		return;
	}
	g_blend = blendMode;
	g_depthTest = 0;
}

void GR_EnableDepth(int enable) { g_depthTest = enable; }
void GR_SetStencilMode(int drawPrim) { g_stencilMode = drawPrim ? 1 : 0; }

void GR_SetPolygonOffset(float ofs)
{
	g_polyOffset = ofs != 0.0f;
	g_polyOffsetVal = ofs;
}

void GR_SetViewPort(int x, int y, int width, int height)
{
	// OpenGL's viewport (origin at the bottom) as a flipped Vulkan one: clip y up stays up
	const float targetH = (float)g_color.h;
	g_viewport = { (float)x, targetH - (float)y, (float)width, -(float)height, 0.0f, 1.0f };
}

void GR_SetWireframe(int) {}

void GR_SetScissorState(int enable) { g_scissorOn = enable; }

void GR_SetupClipMode(const RECT16* rect, int enable)
{
	const bool scissorOn = enable && (activeDispEnv.isinter ||
		(rect->x - activeDispEnv.disp.x > 0 || rect->y - activeDispEnv.disp.y > 0 ||
			rect->w < activeDispEnv.disp.w - 1 || rect->h < activeDispEnv.disp.h - 1));
	GR_SetScissorState(scissorOn);
	if (!scissorOn) return;
	const float emuScreenAspect = 1.0f / (PSX_SCREEN_ASPECT * (float)g_windowWidth / (float)g_windowHeight);
	const float psxScreenWInv = 1.0f / (float)activeDispEnv.disp.w;
	const float psxScreenHInv = 1.0f / (float)activeDispEnv.disp.h;
	float clipRectX = (float)(rect->x - activeDispEnv.disp.x) * psxScreenWInv;
	float clipRectY = (float)(rect->y - activeDispEnv.disp.y) * psxScreenHInv;
	float clipRectW = (float)(rect->w) * psxScreenWInv;
	float clipRectH = (float)(rect->h) * psxScreenHInv;
	clipRectX -= 0.5f;
	clipRectX *= emuScreenAspect;
	clipRectW *= emuScreenAspect;
	clipRectX += 0.5f;
	// Vulkan's scissor origin is the top: the game's clip y is already measured from the top
	int sx = (int)(clipRectX * (float)g_windowWidth);
	int sy = (int)(clipRectY * (float)g_windowHeight);
	int sw = (int)(clipRectW * (float)g_windowWidth);
	int sh = (int)(clipRectH * (float)g_windowHeight);
	if (sx < 0) { sw += sx; sx = 0; }
	if (sy < 0) { sh += sy; sy = 0; }
	if (sw < 0) sw = 0;
	if (sh < 0) sh = 0;
	g_scissor = { { sx, sy }, { (uint32_t)sw, (uint32_t)sh } };
}

void GR_SetOffscreenState(const RECT16* offscreenRect, int enable)
{
	// The offscreen target (a picture the game draws into VRAM) is a later step: until then the matrices
	// are the ones the screen needs.
	g_skipDraws = enable != 0;
	if (enable) g_pfOffscreen++;
	if (enable) {
		GR_Ortho2D(-0.5f, 0.5f, 0.5f, -0.5f, -1.0f, 1.0f);
	} else {
		const float perspectiveFOV = 0.9265f, zNear = 0.25f, zFar = 6000.0f;
		const float emuScreenAspect = (float)(g_windowWidth) / (float)(g_windowHeight);
		GR_Ortho2D(-0.5f * emuScreenAspect * PSX_SCREEN_ASPECT, 0.5f * emuScreenAspect * PSX_SCREEN_ASPECT, 0.5f, -0.5f, -1.0f, 1.0f);
		GR_Perspective3D(perspectiveFOV, 1.0f, 1.0f / (emuScreenAspect * PSX_SCREEN_ASPECT), zNear, zFar);
	}
}

void GR_Clear(int, int, int, int, unsigned char r, unsigned char g, unsigned char b)
{
	framebuffer_need_update = 1;
	ensureRendering();
	VkClearAttachment ca[2]{};
	ca[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	ca[0].clearValue.color = { { r / 255.0f, g / 255.0f, b / 255.0f, 1.0f } };
	ca[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	ca[1].clearValue.depthStencil = { 1.0f, 0 };
	VkClearRect cr{ { { 0, 0 }, { g_color.w, g_color.h } }, 0, 1 };
	vkCmdClearAttachments(g_cmd, 2, ca, 1, &cr);
}

void GR_ClearVRAM(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b)
{
	vram_need_update = 1;
	markDirty(x, y, w, h);
	unsigned short* dst = vram + x + y * VRAM_WIDTH;
	if (x + w > VRAM_WIDTH) w = VRAM_WIDTH - x;
	if (y + h > VRAM_HEIGHT) h = VRAM_HEIGHT - y;
	for (int i = 0; i < h; i++) {
		unsigned short* tmp = dst;
		for (int j = 0; j < w; j++) *tmp++ = r | (g << 5) | (b << 11);
		dst += VRAM_WIDTH;
	}
}

void GR_CopyVRAM(unsigned short* src, int x, int y, int w, int h, int dst_x, int dst_y)
{
	vram_need_update = 1;
	markDirty(dst_x, dst_y, w, h);
	int stride = w;
	if (!src) {
		framebuffer_need_update = 1;
		src = vram;
		stride = VRAM_WIDTH;
	}
	src += x + y * stride;
	unsigned short* dst = vram + dst_x + dst_y * VRAM_WIDTH;
	for (int i = 0; i < h; i++) {
		memmove(dst, src, w * sizeof(short));
		dst += VRAM_WIDTH;
		src += stride;
	}
}

void GR_ReadVRAM(unsigned short* dst, int x, int y, int dst_w, int dst_h)
{
	unsigned short* src = vram + x + VRAM_WIDTH * y;
	for (int i = 0; i < dst_h; i++) {
		memcpy(dst, src, dst_w * sizeof(short));
		dst += dst_w;
		src += VRAM_WIDTH;
	}
}

void GR_StoreFrameBuffer(int x, int y, int w, int h)
{
	g_pfStore++;
	if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > VRAM_WIDTH || y + h > VRAM_HEIGHT) return;
	openFrame();
	endRendering();
	barrier(g_cmd, g_color.image, VK_IMAGE_ASPECT_COLOR_BIT, g_colorLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	g_colorLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier(g_cmd, g_fbSmall.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	VkImageBlit bl{};
	bl.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	bl.srcOffsets[1] = { (int32_t)g_color.w, (int32_t)g_color.h, 1 };
	bl.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	bl.dstOffsets[1] = { w, h, 1 };
	vkCmdBlitImage(g_cmd, g_color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_fbSmall.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
	barrier(g_cmd, g_fbSmall.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	VkBufferImageCopy c{};
	c.bufferRowLength = w;
	c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	c.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
	vkCmdCopyImageToBuffer(g_cmd, g_fbSmall.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_fbRead[g_slot].buffer, 1, &c);
	g_fbPending[g_slot] = { true, x, y, w, h };
}
void GR_UpdateVRAM()
{
	if (!vram_need_update) return;
	openFrame();
	uploadVram();
}
void GR_ReadFramebufferDataToVRAM() { g_pfReadFb++; }

void GR_UpdateVertexBuffer(const GrVertex* vertices, int num_vertices)
{
	if (g_pgxpVertexIndex > g_pfPgxpMax) g_pfPgxpMax = g_pgxpVertexIndex;
	if (g_pgxpVertexIndex > (1u << 21) - 4096) g_pfPgxpOver++;
	if ((unsigned)num_vertices > g_pfVtxMax) g_pfVtxMax = (unsigned)num_vertices;
	// the game's projection constants, as its own 3D vertices carry them: the sky and the far field need the same
	for (int i = 0, n = num_vertices < 6000 ? num_vertices : 6000; i < n; i++) {
		if (vertices[i].scr_h > 100.0f) {
			if (g_fx.skyH != vertices[i].scr_h || g_fx.farOfsX != vertices[i].ofsX || g_fx.farOfsY != vertices[i].ofsY) {
				g_fx.skyH = vertices[i].scr_h; g_fx.farOfsX = vertices[i].ofsX; g_fx.farOfsY = vertices[i].ofsY;
				g_projDirty = true;
			}
			break;
		}
	}
	openFrame();
	if (num_vertices > MAX_VERTEX_BUFFER_SIZE) num_vertices = MAX_VERTEX_BUFFER_SIZE;
	if (g_vtxCursor + (uint32_t)num_vertices > (uint32_t)MAX_VERTEX_BUFFER_SIZE) {
		LOGF("vulkan: vertex ring full, dropping a batch\n");
		g_vtxBase = 0;
		num_vertices = 0;
	} else {
		g_vtxBase = g_vtxCursor;
		memcpy(g_vtxMap[g_slot] + g_vtxBase, vertices, (size_t)num_vertices * sizeof(GrVertex));
		g_vtxCursor += num_vertices;
	}
}

void GR_PushDebugLabel(const char*) {}
void GR_PopDebugLabel() {}

static void drawSkyPass();
static bool g_skyPending = false;
static void drawFarPass();
static bool g_farPending = false;

// a new slot of the frame's parameters when anything changed since the last draw; the slot's dynamic offset
static uint32_t commitFx()
{
	if (g_projDirty) {
		if (g_uboSlot >= kUboSlots) g_uboSlot = kUboSlots - 1;
		memcpy(g_uboMap[g_slot] + (size_t)g_uboSlot * kFxStride, &g_fx, sizeof(FxUBO));
		g_uboSlot++;
		g_projDirty = false;
	}
	return (g_uboSlot - 1) * kFxStride;
}

void GR_DrawTriangles(int start_vertex, int triangles)
{
	if (g_skipDraws) { g_pfSkipped += triangles; return; }
	g_pfDraws++; g_pfVerts += triangles * 3;
	ensureRendering();
	if (g_skyPending && !g_inShadow && !g_prepassKind) {
		if (g_traceDraws >= 0) LOGF("trace: --- sky pass drawn (before draw %d) ---\n", g_traceDraws);
		drawSkyPass();
	}
	if (g_farPending && !g_inShadow && !g_prepassKind)
		drawFarPass();
	if (g_traceDraws >= 0 && g_traceDraws < 400) {
		LOGF("trace: #%d kind=%d tex=%u mode=%d blend=%d depth=%d prepassColour=%d stencil=%d tris=%d start=%d pass=%d\n", g_traceDraws,
			currentKind(), g_curTex, g_texMode, g_blend, g_depthTest, (int)g_prepassColour, g_stencilMode, triangles, start_vertex, g_inShadow ? 1 : (g_prepassKind ? 3 : g_pass));
	}
	if (g_traceDraws >= 0) g_traceDraws++;
	flushState();
	const uint32_t dynOffset = commitFx();
	const TextureID tid = (g_curTex < g_tex.size() && g_tex[g_curTex].alive) ? g_curTex : kTexVram;
	VkDescriptorSet texSet = (tid == kTexVram) ? g_vramSet : g_tex[tid].set;
	VkDescriptorSet sets[3] = { texSet, g_frameSet[g_slot], g_fxSet };
	vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeLayout, 0, 3, sets, 1, &dynOffset);
	struct { int texMode; int bilinear; float texel[2]; int pass; int cascade; int cutout; int pad; } pc =
		{ g_texMode, g_cfg_bilinearFiltering, { g_texel[0], g_texel[1] }, g_inShadow ? 1 : (g_prepassKind ? 3 : g_pass), g_cascade, g_cutout, 0 };
	vkCmdPushConstants(g_cmd, g_pipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	const VkDeviceSize off = (VkDeviceSize)g_vtxBase * sizeof(GrVertex);
	vkCmdBindVertexBuffers(g_cmd, 0, 1, &g_vtxBuf[g_slot], &off);
	vkCmdDraw(g_cmd, triangles * 3, 1, start_vertex, 0);
}

} // extern "C"

void GR_UpdateVertexBufferRange(int start, const GrVertex* vertices, int num_vertices)
{
	openFrame();
	if (g_vtxBase + (uint32_t)start + (uint32_t)num_vertices > (uint32_t)MAX_VERTEX_BUFFER_SIZE) return;
	memcpy(g_vtxMap[g_slot] + g_vtxBase + start, vertices, (size_t)num_vertices * sizeof(GrVertex));
}

void GR_CopyRGBAFramebufferToVRAM(unsigned int*, int, int, int, int, int, int) {}

extern "C" void PsyX_GetPSXWidescreenMappedViewport(struct _RECT16* r)
{
	RECT16* rect = (RECT16*)r;
	const float emuScreenAspect = (float)(g_windowWidth) / (float)(g_windowHeight);
	const float psxScreenW = activeDispEnv.disp.w;
	const float psxScreenH = activeDispEnv.disp.h;
	rect->x = activeDispEnv.screen.x;
	rect->y = activeDispEnv.screen.y;
	rect->w = psxScreenW * emuScreenAspect * PSX_SCREEN_ASPECT;
	rect->h = psxScreenH;
	rect->x -= (rect->w - activeDispEnv.disp.w) / 2;
	rect->w += rect->x;
}

// ---------------------------------------------------------------------------------------------
// the PS5 look of the OpenGL renderer: shadow cascades and headlight maps, lights, fog, wet roads, the depth prepass
// (HD textures and the bloom/SSAO present are the next steps)
// ---------------------------------------------------------------------------------------------
extern "C" int PS5_LightsNearest(int maxN, float* a, float* b, float* c, int* group);

static float g_shM[9];
static int g_shValid = 0;
static float g_spotVPm[3][16];
static int g_spotCount = 0;
static float g_fxBloomDefault = -1.0f, g_fxSSAODefault = -1.0f;
static int g_hdUserOn = 1;
static unsigned int g_hdMaskBits = 0;
static int HDMaskEff() { return (g_ps5ModOn && g_hdUserOn) ? (int)g_hdMaskBits : 0; }

static void initEffects()
{
	int S = g_cfg_ps5ShadowSize;
	if (S < 1024) S = 1024;
	if (S > 8192) S = 8192;
	g_cfg_ps5ShadowSize = S;
	g_shadowReady = g_cfg_ps5Shadows != 0;
	const uint32_t shadowDim = g_shadowReady ? (uint32_t)S : 4, spotDim = g_shadowReady ? (uint32_t)kSpotSize : 4;
	createImageArray(g_shadowImg, shadowDim, shadowDim, 3, 1, kShadowFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, true);
	createImageArray(g_spotImg, spotDim, spotDim, 3, 1, kShadowFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, true);
	const uint32_t hdDim = g_cfg_ps5HD ? 512 : 4, hdMips = g_cfg_ps5HD ? 3 : 1;
	createImageArray(g_hdImg, hdDim, hdDim, 32, hdMips, VK_FORMAT_R8G8B8A8_UNORM,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, false);

	VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	si.magFilter = si.minFilter = VK_FILTER_LINEAR;
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.compareEnable = VK_TRUE;
	si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	si.maxLod = 0.25f;
	check(vkCreateSampler(H->device, &si, nullptr, &g_samplerShadow), "shadow sampler");
	si.compareEnable = VK_FALSE;
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	si.maxLod = 8.0f;
	check(vkCreateSampler(H->device, &si, nullptr, &g_samplerHD), "hd sampler");

	// every image starts in the layout its descriptor names
	VkCommandBuffer cmd = beginOneTime();
	barrierRange(cmd, g_shadowImg.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, 3);
	barrierRange(cmd, g_spotImg.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, 3);
	barrierRange(cmd, g_hdImg.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, 32, 0, hdMips);
	endOneTime(cmd);

	VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	ai.descriptorPool = g_descPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &g_fxSetLayout;
	check(vkAllocateDescriptorSets(H->device, &ai, &g_fxSet), "fx set");
	VkDescriptorImageInfo ii[3] = {
		{ g_samplerShadow, g_shadowImg.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
		{ g_samplerShadow, g_spotImg.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
		{ g_samplerHD, g_hdImg.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
	};
	VkWriteDescriptorSet w[3]{};
	for (int i = 0; i < 3; i++) {
		w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[i].dstSet = g_fxSet; w[i].dstBinding = i; w[i].descriptorCount = 1;
		w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[i].pImageInfo = &ii[i];
	}
	vkUpdateDescriptorSets(H->device, 3, w, 0, nullptr);

	g_fx.shadowSize = (float)S;
	g_fx.shadowStrength = g_cfg_ps5ShadowStrength;
	g_fx.shadowBias = 0.0008f;
	g_fx.shadowDebug = g_cfg_ps5ShadowDebug;
	g_fx.shadowOn = g_shadowReady ? 1 : 0;
	g_projDirty = true;
	LOGF("vulkan: effects ready (shadows %s %d, lights %d, fog %d, HD %d, prepass %d)\n", g_shadowReady ? "on" : "off", S, g_cfg_ps5Lights, g_cfg_ps5Fog, g_cfg_ps5HD, g_cfg_ps5Prepass);
}

// sun: world vector pointing from the scene towards the sun (the game's sun_position); m: the game's inv_camera_matrix
// (4096 fixed point, view = M * (world - camera)).
void GR_PS5_SetShadowParams(int sx, int sy, int sz, const short* m, int cx, int cy, int cz)
{
	float M[9];
	for (int i = 0; i < 9; i++) M[i] = (float)m[i] / 4096.0f;
	// world = M^T * view: the row-major M read as the columns of a mat3 is exactly M^T
	for (int c = 0; c < 3; c++) {
		g_fx.viewToWorld[c * 4 + 0] = M[c * 3 + 0];
		g_fx.viewToWorld[c * 4 + 1] = M[c * 3 + 1];
		g_fx.viewToWorld[c * 4 + 2] = M[c * 3 + 2];
		g_fx.viewToWorld[c * 4 + 3] = 0.0f;
	}
	for (int i = 0; i < 9; i++) g_shM[i] = M[i];
	g_projDirty = true;
	if (!g_cfg_ps5Shadows || !g_shadowReady)
		return;

	// light travels from the sun towards the scene
	const float flip = g_cfg_ps5ShadowFlip ? -1.0f : 1.0f;
	float L[3] = { -(float)sx * flip, -(float)sy * flip, -(float)sz * flip };
	float ll = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
	if (ll < 1.0f) return;
	for (int i = 0; i < 3; i++) L[i] /= ll;
	float up[3] = { 0.0f, 1.0f, 0.0f };
	if (fabsf(L[1]) > 0.99f) { up[0] = 1.0f; up[1] = 0.0f; }
	float lx[3] = { up[1] * L[2] - up[2] * L[1], up[2] * L[0] - up[0] * L[2], up[0] * L[1] - up[1] * L[0] };
	float lxl = sqrtf(lx[0] * lx[0] + lx[1] * lx[1] + lx[2] * lx[2]);
	for (int i = 0; i < 3; i++) lx[i] /= lxl;
	float ly[3] = { L[1] * lx[2] - L[2] * lx[1], L[2] * lx[0] - L[0] * lx[2], L[0] * lx[1] - L[1] * lx[0] };

	// three cascades, each a square of half width E centred half a width in front of the camera (camera-relative
	// world space), the depth range along the light is three times E
	static const float cascadeE[3] = { 6000.0f, 20000.0f, 60000.0f };
	for (int ci = 0; ci < 3; ci++) {
		const float E = cascadeE[ci];
		const float R = E * 3.0f;
		const float focusDist = E * 0.5f;
		float focus[3] = { M[6] * focusDist, M[7] * focusDist, M[8] * focusDist };
		// snap the focus to the shadow-map texel grid in absolute world space so shadows do not swim as the camera moves
		{
			const float texel = 2.0f * E / (float)g_cfg_ps5ShadowSize;
			const float fa[3] = { focus[0] + cx, focus[1] + cy, focus[2] + cz };
			const float a = lx[0] * fa[0] + lx[1] * fa[1] + lx[2] * fa[2];
			const float b = ly[0] * fa[0] + ly[1] * fa[1] + ly[2] * fa[2];
			const float da = floorf(a / texel + 0.5f) * texel - a;
			const float db = floorf(b / texel + 0.5f) * texel - b;
			for (int i = 0; i < 3; i++) focus[i] += lx[i] * da + ly[i] * db;
		}
		float rows[3][3] = { { lx[0], lx[1], lx[2] }, { ly[0], ly[1], ly[2] }, { L[0], L[1], L[2] } };
		const float scale[3] = { 1.0f / E, 1.0f / E, 1.0f / R };
		float* vp = g_fx.lightVP[ci];
		for (int r = 0; r < 3; r++) {
			const float d = rows[r][0] * focus[0] + rows[r][1] * focus[1] + rows[r][2] * focus[2];
			for (int c = 0; c < 3; c++)
				vp[c * 4 + r] = rows[r][c] * scale[r];
			vp[3 * 4 + r] = -d * scale[r];
		}
		vp[0 * 4 + 3] = vp[1 * 4 + 3] = vp[2 * 4 + 3] = 0.0f;
		vp[3 * 4 + 3] = 1.0f;
	}
	g_shValid = 1;
}

void GR_PS5_SetShadowCutout(int on) { g_cutout = on; }

void GR_PS5_SetShadowDisp(float w, float h)
{
	if (g_fx.dispW != w || g_fx.dispH != h) { g_fx.dispW = w; g_fx.dispH = h; g_projDirty = true; }
}

// selects the cascade (a layer of the shadow texture), clears it and makes the vertex shader project into it
void GR_PS5_ShadowCascade(int c)
{
	beginDepthRendering(g_shadowImg, c, g_cfg_ps5ShadowSize);
	g_cascade = c;
}

void GR_PS5_SpotCascade(int k)
{
	beginDepthRendering(g_spotImg, k, kSpotSize);
	g_cascade = 3 + k;
}

int GR_PS5_ShadowBegin()
{
	if (!g_cfg_ps5Shadows || !g_shValid || !g_shadowReady || !g_ps5ModOn)
		return 0;
	g_shValid = 0;
	openFrame();
	endRendering();
	g_inShadow = true;
	return 1;
}

void GR_PS5_ShadowEnd()
{
	endRendering();
	g_inShadow = false;
	g_passNow = 2;
	g_pass = 2;
	g_projDirty = true;
}

void GR_PS5_ShadowFinish()
{
	if (g_cfg_ps5Shadows && g_shadowReady) { g_passNow = 0; g_pass = 0; }
}

// the lights nearest to the camera (called before the frame's scene batch is drawn) and the headlights' shadow maps
static void SpotViewProj(const float* pos, const float* dir, float radius, float* out)
{
	float f[3] = { dir[0], dir[1], dir[2] };
	float up[3] = { 0.0f, -1.0f, 0.0f };
	if (fabsf(f[1]) > 0.98f) { up[0] = 1.0f; up[1] = 0.0f; }
	float r[3] = { f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0] };
	float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
	for (int i = 0; i < 3; i++) r[i] /= rl;
	float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
	const float n = 60.0f, fa = radius, ft = 1.0f / tanf(0.5f * 1.75f);	// 100 degree field of view
	const float V[16] = {
		r[0], u[0], -f[0], 0.0f,
		r[1], u[1], -f[1], 0.0f,
		r[2], u[2], -f[2], 0.0f,
		-(r[0] * pos[0] + r[1] * pos[1] + r[2] * pos[2]), -(u[0] * pos[0] + u[1] * pos[1] + u[2] * pos[2]), (f[0] * pos[0] + f[1] * pos[1] + f[2] * pos[2]), 1.0f };
	const float P[16] = {
		ft, 0, 0, 0,
		0, ft, 0, 0,
		0, 0, (fa + n) / (n - fa), -1.0f,
		0, 0, 2.0f * fa * n / (n - fa), 0 };
	for (int c = 0; c < 4; c++)
		for (int rr = 0; rr < 4; rr++) {
			float sum = 0.0f;
			for (int k = 0; k < 4; k++)
				sum += P[k * 4 + rr] * V[c * 4 + k];
			out[c * 4 + rr] = sum;
		}
}

void GR_PS5_UploadLights()
{
	if (!g_cfg_ps5Lights)
		return;
	if (!g_ps5ModOn) {
		g_spotCount = 0;
		g_fx.lightN = 0;
		g_projDirty = true;
		return;
	}
	float a[64], b[64], c[64];
	int group[16] = { 0 };
	memset(a, 0, sizeof(a)); memset(b, 0, sizeof(b)); memset(c, 0, sizeof(c));
	const int n = PS5_LightsNearest(16, a, b, c, group);
	for (int i = 0; i < n; i++)
		a[i * 4 + 3] *= g_cfg_ps5LightRadius;
	// the first three cars (by distance) get a shadow map; their lights share it
	g_spotCount = 0;
	if (g_shadowReady && g_cfg_ps5Shadows) {
		int seen[3] = { 0, 0, 0 };
		for (int i = 0; i < n; i++) {
			if (group[i] == 0 || c[i * 4 + 3] < 0.5f)
				continue;
			int k = -1;
			for (int j = 0; j < g_spotCount; j++)
				if (seen[j] == group[i]) k = j;
			if (k < 0 && g_spotCount < 3) {
				k = g_spotCount++;
				seen[k] = group[i];
				SpotViewProj(&a[i * 4], &c[i * 4], a[i * 4 + 3], g_spotVPm[k]);
			}
			if (k >= 0)
				c[i * 4 + 3] = 2.0f + (float)k;
		}
	}
	memcpy(g_fx.lightA, a, sizeof(a));
	memcpy(g_fx.lightB, b, sizeof(b));
	memcpy(g_fx.lightC, c, sizeof(c));
	g_fx.lightStrength = g_cfg_ps5LightStrength;
	g_fx.lightN = n;
	if (g_spotCount > 0)
		memcpy(g_fx.spotVP, g_spotVPm, sizeof(g_spotVPm));
	g_projDirty = true;
}

int GR_PS5_SpotCount() { return g_spotCount; }

// colour of the haze: the configured one scaled by the game's sky brightness (so it darkens at night)
void GR_PS5_SetFog(int skyR, int skyG, int skyB)
{
	if (!g_cfg_ps5Fog)
		return;
	if (!g_ps5ModOn) { g_fx.fogOn = 0; g_projDirty = true; return; }
	const float k[3] = { skyR / 128.0f, skyG / 128.0f, skyB / 128.0f };
	const float c[3] = { g_cfg_ps5FogR / 255.0f, g_cfg_ps5FogG / 255.0f, g_cfg_ps5FogB / 255.0f };
	g_fx.fogR = c[0] * (k[0] > 1.0f ? 1.0f : k[0]);
	g_fx.fogG = c[1] * (k[1] > 1.0f ? 1.0f : k[1]);
	g_fx.fogB = c[2] * (k[2] > 1.0f ? 1.0f : k[2]);
	// cells -> shader units (2048 world units per cell, 128 units per shader unit)
	g_fx.fogStart = g_cfg_ps5FogStart * 16.0f;
	g_fx.fogEnd = g_cfg_ps5FogEnd * 16.0f;
	g_fx.fogOn = 1;
	g_projDirty = true;
}

// the pause menu switch for the whole PS5 look
void GR_PS5_SetModOn(int on)
{
	g_ps5ModOn = on;
	if (!on) { g_fx.fogOn = 0; g_fx.lightN = 0; }
	g_fx.hdMask = HDMaskEff();
	g_projDirty = true;
}

// wetness of the roads, 0..1 (the game's rain level)
void GR_PS5_SetWet(float w)
{
	if (!g_ps5ModOn || !g_cfg_ps5Wet) w = 0.0f;
	g_fx.wet = w;
	g_projDirty = true;
}

// run-time switches of each effect (pause menu: PS5 EFFECTS)
void GR_PS5_SetFeature(int id, int on)
{
	switch (id) {
	case 0: if (!on || g_shadowReady) g_cfg_ps5Shadows = on; g_fx.shadowOn = (g_cfg_ps5Shadows && g_shadowReady) ? 1 : 0; break;
	case 1: g_cfg_ps5Lights = on; if (!on) g_fx.lightN = 0; break;
	case 2: g_hdUserOn = on; g_fx.hdMask = HDMaskEff(); break;
	case 3: g_cfg_ps5Fog = on; if (!on) g_fx.fogOn = 0; break;
	case 4: if (g_fxBloomDefault < 0.0f) g_fxBloomDefault = g_cfg_ps5Bloom > 0.0f ? g_cfg_ps5Bloom : 0.45f; g_cfg_ps5Bloom = on ? g_fxBloomDefault : 0.0f; break;
	case 5: if (g_fxSSAODefault < 0.0f) g_fxSSAODefault = g_cfg_ps5SSAO > 0.0f ? g_cfg_ps5SSAO : 0.6f; g_cfg_ps5SSAO = on ? g_fxSSAODefault : 0.0f; break;
	case 6: g_cfg_ps5Wet = on ? 1.0f : 0.0f; break;
	case 7: g_cfg_ps5Prepass = on; break;
	}
	g_projDirty = true;
}

int GR_PS5_GetFeature(int id)
{
	switch (id) {
	case 0: return g_cfg_ps5Shadows != 0;
	case 1: return g_cfg_ps5Lights != 0;
	case 2: return g_hdUserOn;
	case 3: return g_cfg_ps5Fog != 0;
	case 4: return g_cfg_ps5Bloom > 0.0f;
	case 5: return g_cfg_ps5SSAO > 0.0f;
	case 6: return g_cfg_ps5Wet > 0.0f;
	case 7: return g_cfg_ps5Prepass != 0;
	}
	return 0;
}

void GR_PS5_PerfCpu(int which, double ms) { if (which >= 0 && which < 4) g_pfCpuAcc[which] += ms; }
void GR_PS5_PerfInfo(float* c, float* g) { if (c) *c = 0; if (g) *g = 0; }

// depth prepass: the opaque geometry is drawn twice, first depth only and then in colour only where the depth is equal, so
// the expensive fragment shader runs once per pixel (the game paints back to front)
// Off for good: the colour pass tests depth EQUAL against the prepass, and on this driver the two pipelines do not give
// bit-identical depth for the geometry near the camera, so the nearest ground, the cars and the menus vanished.
int GR_PS5_PrepassUsable() { return 0; }
void GR_PS5_PrepassBegin() { g_prepassKind = 2; g_depthTest = 1; }
void GR_PS5_PrepassEnd() { g_prepassKind = 0; }
void GR_PS5_PrepassColour(int on)
{
	g_prepassColour = on != 0;
	if (on) g_depthTest = 1;
}

// ---- HD texture packs: a texture page of the level (256x256, 4 bit) can be replaced by an upscaled RGBA one ----
#define HD_PAGE_SIZE 512
#define HD_LAYERS 32
extern "C" unsigned char* PsyX_LoadPNG(const char* path, int* w, int* h);
static int g_hdPageX[HD_LAYERS], g_hdPageY[HD_LAYERS];

// one page into its layer, with the two smaller mip levels (done now, outside the frame: pages load with the level)
static void hdUploadLayer(int layer, const unsigned char* px)
{
	Staging st;
	createStaging(st, (VkDeviceSize)HD_PAGE_SIZE * HD_PAGE_SIZE * 4);
	memcpy(st.mapped, px, (size_t)HD_PAGE_SIZE * HD_PAGE_SIZE * 4);
	VkCommandBuffer cmd = beginOneTime();
	barrierRange(cmd, g_hdImg.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, layer, 1, 0, 3);
	VkBufferImageCopy c{};
	c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)layer, 1 };
	c.imageExtent = { HD_PAGE_SIZE, HD_PAGE_SIZE, 1 };
	vkCmdCopyBufferToImage(cmd, st.buffer, g_hdImg.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
	for (int lvl = 1; lvl < 3; lvl++) {
		barrierRange(cmd, g_hdImg.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, layer, 1, lvl - 1, 1);
		VkImageBlit bl{};
		bl.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)(lvl - 1), (uint32_t)layer, 1 };
		bl.srcOffsets[1] = { HD_PAGE_SIZE >> (lvl - 1), HD_PAGE_SIZE >> (lvl - 1), 1 };
		bl.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)lvl, (uint32_t)layer, 1 };
		bl.dstOffsets[1] = { HD_PAGE_SIZE >> lvl, HD_PAGE_SIZE >> lvl, 1 };
		vkCmdBlitImage(cmd, g_hdImg.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_hdImg.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
	}
	barrierRange(cmd, g_hdImg.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, layer, 1, 0, 2);
	barrierRange(cmd, g_hdImg.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, layer, 1, 2, 1);
	endOneTime(cmd);
	vkDestroyBuffer(H->device, st.buffer, nullptr);
	vkFreeMemory(H->device, st.memory, nullptr);
}

// Called when level texture page `page` has just been loaded into the VRAM page at (vx, vy). `city` is the level name
// (NULL when the pack does not apply, e.g. at night): the HD replacement is looked up in DRIVER2/HD/<city>/PAGE_<n>.png.
void GR_PS5_HDPageLoaded(const char* city, int vx, int vy, int page)
{
	if (!g_cfg_ps5HD || g_hdImg.w != HD_PAGE_SIZE)
		return;
	const int key = (vx >> 6) + ((vy >> 8) << 4);
	if (key < 0 || key >= HD_LAYERS)
		return;
	g_hdPageX[key] = vx;
	g_hdPageY[key] = vy;
	g_fx.hdHole[key][0] = g_fx.hdHole[key][1] = 1e9f;
	g_fx.hdHole[key][2] = g_fx.hdHole[key][3] = -1e9f;
	unsigned char* px = NULL;
	int w = 0, h = 0;
	if (city) {
		char path[96];
		snprintf(path, sizeof(path), "DRIVER2/HD/%s/PAGE_%d.png", city, page);
		px = PsyX_LoadPNG(path, &w, &h);
	}
	if (px && w == HD_PAGE_SIZE && h == HD_PAGE_SIZE) {
		hdUploadLayer(key, px);
		g_hdMaskBits |= 1u << key;
	} else
		g_hdMaskBits &= ~(1u << key);
	free(px);
	g_fx.hdMask = HDMaskEff();
	g_projDirty = true;
}

// Something wrote the VRAM rectangle (x, y, w, h in 16 bit words): where it lands on an HD page the original texels must be
// used (the game updates some textures at run time: the minimap, animated water...), so the area becomes a hole of the HD page.
void GR_PS5_HDVramWrite(int x, int y, int w, int h)
{
	if (!g_hdMaskBits)
		return;
	bool changed = false;
	for (int key = 0; key < HD_LAYERS; key++) {
		if (!(g_hdMaskBits & (1u << key)))
			continue;
		const int px = g_hdPageX[key], py = g_hdPageY[key];
		if (x >= px + 64 || x + w <= px || y >= py + 256 || y + h <= py)
			continue;
		// page-relative texels (4 bit pages: 4 texels per word)
		const float x0 = (float)((x > px ? x : px) - px) * 4.0f;
		const float x1 = (float)((x + w < px + 64 ? x + w : px + 64) - px) * 4.0f;
		const float y0 = (float)((y > py ? y : py) - py);
		const float y1 = (float)((y + h < py + 256 ? y + h : py + 256) - py);
		float* hole = g_fx.hdHole[key];
		if (x0 < hole[0]) { hole[0] = x0; changed = true; }
		if (y0 < hole[1]) { hole[1] = y0; changed = true; }
		if (x1 > hole[2]) { hole[2] = x1; changed = true; }
		if (y1 > hole[3]) { hole[3] = y1; changed = true; }
	}
	if (changed) g_projDirty = true;
}

int GR_PS5_PresentShader() { return 0; }
int GR_PS5_PresentTexLoc() { return 0; }
void GR_PS5_InitShadows() {}

// ---------------------------------------------------------------------------------------------
// the HD sky: panoramas of the city (DRIVER2/HD/SKY), drawn before the scene instead of the PSX dome
// ---------------------------------------------------------------------------------------------
extern int PS5_GteH();
extern "C" unsigned char* PsyX_LoadPNG(const char* path, int* w, int* h);

struct SkyPano {
	std::string name;
	Image img;
	VkDescriptorSet set = VK_NULL_HANDLE;
	float sunU = 0.6f, sunElev = 15.0f;
	float fog[3] = { 190, 205, 225 };
	bool night = false;
	bool loaded = false, failed = false;
};
static std::vector<SkyPano> g_skyPanos;
static std::vector<std::pair<std::string, std::string>> g_skyMap;   // "CITY.moment" -> panorama name
#define g_skyEnabled (g_cfg_hdSky != 0)
static bool g_skyIniRead = false;
static int g_skyCur = -1;
static float g_skyYaw = 0.0f, g_skyBright = 1.0f;
static VkPipeline g_skyPipe = VK_NULL_HANDLE;
static VkShaderModule g_svs, g_sfs;
static VkSampler g_samplerPano;

static SkyPano* skyFind(const std::string& name, bool create)
{
	for (auto& p : g_skyPanos)
		if (p.name == name) return &p;
	if (!create) return nullptr;
	g_skyPanos.emplace_back();
	g_skyPanos.back().name = name;
	return &g_skyPanos.back();
}

static void skyReadIni()
{
	g_skyIniRead = true;
	FILE* f = fopen("/app0/assets/DRIVER2/HD/SKY/sky.ini", "rb");
	if (!f) { LOGF("sky: no DRIVER2/HD/SKY/sky.ini, the PSX sky stays\n"); return; }
	char line[256], section[96] = "";
	while (fgets(line, sizeof(line), f)) {
		char* e = line + strlen(line);
		while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
		if (line[0] == '#' || !line[0]) continue;
		if (line[0] == '[') { snprintf(section, sizeof(section), "%s", line + 1); char* r = strchr(section, ']'); if (r) *r = 0; continue; }
		char* eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		const char* key = line; const char* val = eq + 1;
		const bool isCity = !strcmp(section, "CHICAGO") || !strcmp(section, "HAVANA") || !strcmp(section, "VEGAS") || !strcmp(section, "RIO");
		if (isCity) { g_skyMap.push_back({ std::string(section) + "." + key, val }); continue; }
		SkyPano* p = skyFind(section, true);
		if (!strcmp(key, "sun_u")) p->sunU = (float)atof(val);
		else if (!strcmp(key, "sun_elev")) p->sunElev = (float)atof(val);
		else if (!strcmp(key, "night")) p->night = atoi(val) != 0;
		else if (!strcmp(key, "fog")) sscanf(val, "%f,%f,%f", &p->fog[0], &p->fog[1], &p->fog[2]);
	}
	fclose(f);
	LOGF("sky: %zu panoramas, %zu city moments\n", g_skyPanos.size(), g_skyMap.size());
}

static void skyBuildPipeline()
{
	g_svs = loadModule("sky.vert.spv");
	g_sfs = loadModule("sky.frag.spv");
	VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	si.magFilter = si.minFilter = VK_FILTER_LINEAR;
	si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	check(vkCreateSampler(H->device, &si, nullptr, &g_samplerPano), "pano sampler");
	VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vp.viewportCount = 1; vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	// the sky sits on the far plane (depth 1) and is drawn only where nothing nearer is: it can come at any moment of the frame
	VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = VK_FALSE;
	ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cb.attachmentCount = 1; cb.pAttachments = &cba;
	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
	VkPipelineShaderStageCreateInfo st[2]{};
	st[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, g_svs, "main" };
	st[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, g_sfs, "main" };
	VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &g_color.format;
	ri.depthAttachmentFormat = g_depth.format; ri.stencilAttachmentFormat = g_depth.format;
	VkGraphicsPipelineCreateInfo pi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pi.pNext = &ri; pi.stageCount = 2; pi.pStages = st;
	pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
	pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds;
	pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = g_pipeLayout;
	check(vkCreateGraphicsPipelines(H->device, H->pipelineCache, 1, &pi, nullptr, &g_skyPipe), "sky pipeline");
}

static void skyLoad(SkyPano& p)
{
	if (p.loaded || p.failed) return;
	char path[160];
	snprintf(path, sizeof(path), "DRIVER2/HD/SKY/%s.png", p.name.c_str());
	int w = 0, h = 0;
	unsigned char* px = PsyX_LoadPNG(path, &w, &h);
	if (!px || w < 64 || h < 32) {
		LOGF("sky: %s missing\n", path);
		free(px);
		p.failed = true;
		return;
	}
	const double t0 = nowMs();
	createImage(p.img, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
	uploadImage(p.img, px, (size_t)w * h * 4, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	free(px);
	p.set = makeTexSet(p.img.view, g_samplerPano);
	p.loaded = true;
	LOGF("sky: %s %dx%d loaded in %.0f ms\n", p.name.c_str(), w, h, nowMs() - t0);
}

int GR_PS5_SkyActive(void) { return g_skyEnabled && g_skyCur >= 0; }

// The scene's sky for this frame: picked by city, moment of the day and weather. Returns 1 when the HD sky will be
// drawn (the game then skips its dome).
int GR_PS5_SkyFrame(const char* city, int timeOfDay, int weather, int sunX, int sunY, int sunZ, int brightR, int brightG, int brightB)
{
	if (!g_skyEnabled || !g_ps5ModOn) { g_skyCur = -1; return 0; }
	if (!g_skyIniRead) skyReadIni();
	if (g_skyMap.empty()) return 0;
	// TIME_DAWN 0, DAY 1, DUSK 2, NIGHT 3; WEATHER_NONE 0, RAIN 1, WET 2
	const char* moment = "day";
	if (timeOfDay == 3) moment = weather ? "night_rain" : "night";
	else if (weather) moment = "day_rain";
	else if (timeOfDay == 0) moment = "dawn";
	else if (timeOfDay == 2) moment = "dusk";
	const std::string key = std::string(city ? city : "") + "." + moment;
	const std::string* name = nullptr;
	for (auto& e : g_skyMap)
		if (e.first == key) { name = &e.second; break; }
	if (!name) { g_skyCur = -1; return 0; }
	SkyPano* p = skyFind(*name, false);
	if (!p) { g_skyCur = -1; return 0; }
	if (!g_skyPipe) skyBuildPipeline();
	skyLoad(*p);
	if (!p->loaded) { g_skyCur = -1; return 0; }
	g_skyCur = (int)(p - g_skyPanos.data());
	// turn the panorama so that its sun sits where the game's sun (the shadows' direction) is
	const float azSun = atan2f((float)sunX, (float)sunZ) * 0.15915494f + 0.5f;
	g_skyYaw = p->sunU - azSun;
	g_skyBright = ((brightR + brightG + brightB) / 3.0f) / 128.0f;
	if (g_skyBright > 1.3f) g_skyBright = 1.3f;
	// the haze takes the panorama's horizon colour (darker for a night)
	if (g_cfg_ps5Fog) {
		const float k = (p->night ? 0.4f : 1.0f) / 255.0f;
		g_fx.fogR = p->fog[0] * k; g_fx.fogG = p->fog[1] * k; g_fx.fogB = p->fog[2] * k;
		g_fx.fogStart = g_cfg_ps5FogStart * 16.0f;
		g_fx.fogEnd = g_cfg_ps5FogEnd * 16.0f;
		g_fx.fogOn = 1;
	}
	g_projDirty = true;
	g_skyPending = true;
	return 1;
}

static void drawSkyPass()
{
	g_skyPending = false;
	if (g_skyCur < 0 || !g_skyPipe) return;
	SkyPano& p = g_skyPanos[g_skyCur];
	vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_skyPipe);
	vkCmdSetViewport(g_cmd, 0, 1, &g_viewport);
	VkRect2D sc{ { 0, 0 }, { g_color.w, g_color.h } };
	vkCmdSetScissor(g_cmd, 0, 1, &sc);
	const uint32_t dynOffset = commitFx();
	VkDescriptorSet sets[3] = { p.set, g_frameSet[g_slot], g_fxSet };
	vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeLayout, 0, 3, sets, 1, &dynOffset);
	struct { int texMode; int bilinear; float yaw; float bright; int pass; int cascade; int cutout; int pad; } pc =
		{ 0, 0, g_skyYaw, g_skyBright, 0, 0, 0, 0 };
	vkCmdPushConstants(g_cmd, g_pipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDraw(g_cmd, 3, 1, 0, 0);
}

// ---------------------------------------------------------------------------------------------
// the far field: static world-space meshes of the map beyond the game's own range (game/libred2vk.a's farmesh.c bakes them)
// ---------------------------------------------------------------------------------------------
struct FarChunk {
	int key = 0, fmt = 0;
	int ox = 0, oy = 0, oz = 0;
	int count = 0;
	VkBuffer buf = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
};
struct FarPending { size_t chunk; size_t offset; size_t size; };
static std::vector<FarChunk> g_farChunks;
static std::vector<FarPending> g_farPendingCopies;
static std::vector<unsigned char> g_farStage;
static std::vector<int> g_farVisible;
static int g_farCam[3];
static VkPipeline g_farPipe = VK_NULL_HANDLE;
static VkShaderModule g_fvs;
static const size_t kFarVertexSize = 20;

static size_t farChunkCount() { return g_farChunks.size(); }

static void farFlush()
{
	if (g_farPendingCopies.empty()) return;
	Staging st;
	createStaging(st, g_farStage.size());
	memcpy(st.mapped, g_farStage.data(), g_farStage.size());
	VkCommandBuffer cmd = beginOneTime();
	for (auto& c : g_farPendingCopies) {
		VkBufferCopy bc{ c.offset, 0, c.size };
		vkCmdCopyBuffer(cmd, st.buffer, g_farChunks[c.chunk].buf, 1, &bc);
	}
	VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
	mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	mb.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT; mb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
	VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
	vkCmdPipelineBarrier2(cmd, &di);
	endOneTime(cmd);
	vkDestroyBuffer(H->device, st.buffer, nullptr);
	vkFreeMemory(H->device, st.memory, nullptr);
	g_farPendingCopies.clear();
	g_farStage.clear();
}

void GR_PS5_FarClear(void)
{
	if (g_farChunks.empty()) return;
	vkDeviceWaitIdle(H->device);
	for (auto& c : g_farChunks) {
		vkDestroyBuffer(H->device, c.buf, nullptr);
		vkFreeMemory(H->device, c.mem, nullptr);
	}
	g_farChunks.clear();
	g_farPendingCopies.clear();
	g_farStage.clear();
	g_farVisible.clear();
}

// one chunk (8x8 cells) of one texture format: its vertices wait for farFlush, which uploads a whole region at once
typedef struct { float x, y, z; unsigned short page, clut; unsigned char u, v, col, flags; } FarVertex;
static_assert(sizeof(FarVertex) == 20, "FarVertex");
void GR_PS5_FarChunkSet(int key, int fmt, int ox, int oy, int oz, const FarVertex* verts, int count)
{
	if (count <= 0) return;
	FarChunk c;
	c.key = key; c.fmt = fmt; c.ox = ox; c.oy = oy; c.oz = oz; c.count = count;
	const size_t bytes = (size_t)count * kFarVertexSize;
	VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = bytes;
	bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	check(vkCreateBuffer(H->device, &bi, nullptr, &c.buf), "far buffer");
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(H->device, c.buf, &mr);
	VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = H->vulkanDevice->getMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	check(vkAllocateMemory(H->device, &ai, nullptr, &c.mem), "far memory");
	check(vkBindBufferMemory(H->device, c.buf, c.mem, 0), "far bind");
	const size_t offset = (g_farStage.size() + 15) & ~(size_t)15;
	g_farStage.resize(offset + bytes);
	memcpy(g_farStage.data() + offset, verts, bytes);
	g_farChunks.push_back(c);
	g_farPendingCopies.push_back({ g_farChunks.size() - 1, offset, bytes });
}

void GR_PS5_FarFlush(void) { farFlush(); }

// one texture page of the level into the far field's store: 256x256 palette indices (0..15) and `npal` palettes of 16
// colours (5551 as the PSX has them)
void GR_PS5_FarPageSet(int page, const unsigned char* idx, int npal, const unsigned short* pal)
{
	if (page < 0 || page >= kFarPages || !idx) return;
	if (npal > kFarPalRows) npal = kFarPalRows;
	const size_t palBytes = (size_t)npal * 16 * 4;
	Staging st;
	createStaging(st, 65536 + (palBytes ? palBytes : 4));
	memcpy(st.mapped, idx, 65536);
	unsigned char* d = (unsigned char*)st.mapped + 65536;
	for (int i = 0; i < npal * 16; i++, d += 4) {
		const unsigned c = pal[i];
		d[0] = (unsigned char)((c & 31) << 3);
		d[1] = (unsigned char)(((c >> 5) & 31) << 3);
		d[2] = (unsigned char)(((c >> 10) & 31) << 3);
		d[3] = (unsigned char)(((c >> 15) & 1) << 7);
	}
	VkCommandBuffer cmd = beginOneTime();
	barrierRange(cmd, g_farPageImg.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, page, 1);
	VkBufferImageCopy c{};
	c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)page, 1 };
	c.imageExtent = { 256, 256, 1 };
	vkCmdCopyBufferToImage(cmd, st.buffer, g_farPageImg.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
	barrierRange(cmd, g_farPageImg.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, page, 1);
	if (npal > 0) {
		barrier(cmd, g_farPalImg.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		VkBufferImageCopy pc{};
		pc.bufferOffset = 65536;
		pc.bufferRowLength = 16;
		pc.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		pc.imageOffset = { 0, page * kFarPalRows, 0 };
		pc.imageExtent = { 16, (uint32_t)npal, 1 };
		vkCmdCopyBufferToImage(cmd, st.buffer, g_farPalImg.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &pc);
		barrier(cmd, g_farPalImg.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	}
	endOneTime(cmd);
	vkDestroyBuffer(H->device, st.buffer, nullptr);
	vkFreeMemory(H->device, st.memory, nullptr);
	g_farPageHave[page] = 1;
}

void GR_PS5_FarDraw(int key) { g_farVisible.push_back(key); }

// once a frame: the camera and the far field's colours (the game's lighting tables as plotted this frame)
void GR_PS5_FarFrame(int camX, int camY, int camZ, const unsigned int* colours)
{
	g_farCam[0] = camX; g_farCam[1] = camY; g_farCam[2] = camZ;
	if (memcmp(g_fx.farCol, colours, sizeof(g_fx.farCol)) != 0) {
		memcpy(g_fx.farCol, colours, sizeof(g_fx.farCol));
		g_projDirty = true;
	}
	g_farPending = !g_farVisible.empty();
}

static void farBuildPipeline()
{
	g_fvs = loadModule("far.vert.spv");
	VkVertexInputBindingDescription bind{ 0, (uint32_t)kFarVertexSize, VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attr[3] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R16G16_UINT, 12 },
		{ 2, 0, VK_FORMAT_R8G8B8A8_UINT, 16 },
	};
	VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
	vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = attr;
	VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vp.viewportCount = 1; vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = g_cfg_farMeshCull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
	rs.frontFace = (g_cfg_farMeshCull == 2) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
	rs.lineWidth = 1.0f;
	rs.depthBiasEnable = VK_TRUE;      // pushed back a little: where it overlaps the game's own drawing, that wins
	VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = VK_TRUE;
	ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cb.attachmentCount = 1; cb.pAttachments = &cba;
	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS };
	VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dy.dynamicStateCount = 3; dy.pDynamicStates = dyn;
	VkPipelineShaderStageCreateInfo st[2]{};
	st[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, g_fvs, "main" };
	st[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, g_fs, "main" };
	VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &g_color.format;
	ri.depthAttachmentFormat = g_depth.format; ri.stencilAttachmentFormat = g_depth.format;
	VkGraphicsPipelineCreateInfo pi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pi.pNext = &ri; pi.stageCount = 2; pi.pStages = st;
	pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
	pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds;
	pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = g_pipeLayout;
	check(vkCreateGraphicsPipelines(H->device, H->pipelineCache, 1, &pi, nullptr, &g_farPipe), "far pipeline");
}

static void drawFarPass()
{
	g_farPending = false;
	if (g_farVisible.empty()) return;
	if (!g_farPipe) farBuildPipeline();
	vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_farPipe);
	vkCmdSetViewport(g_cmd, 0, 1, &g_viewport);
	VkRect2D sc{ { 0, 0 }, { g_color.w, g_color.h } };
	vkCmdSetScissor(g_cmd, 0, 1, &sc);
	vkCmdSetDepthBias(g_cmd, 4.0f, 0.0f, 1.0f);
	const uint32_t dynOffset = commitFx();
	VkDescriptorSet sets[3] = { g_vramSet, g_frameSet[g_slot], g_fxSet };
	vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeLayout, 0, 3, sets, 1, &dynOffset);
	unsigned drawn = 0, verts = 0;
	for (int key : g_farVisible) {
		for (auto& c : g_farChunks) {
			if (c.key != key) continue;
			// the chunk's origin minus the camera (world units): x and y travel in texelSize, z in the spare word
			const float off[3] = { (float)((double)c.ox - g_farCam[0]), (float)((double)c.oy - g_farCam[1]), (float)((double)c.oz - g_farCam[2]) };
			struct { int texMode; int bilinear; float tx, ty; int pass; int cascade; int cutout; int pad; } pc =
				{ 5, g_cfg_bilinearFiltering, off[0], off[1], g_pass, 0, g_cfg_farMeshDebug == 1 ? 2 : 0, 0 };
			memcpy(&pc.pad, &off[2], 4);
			vkCmdPushConstants(g_cmd, g_pipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
			const VkDeviceSize zero = 0;
			vkCmdBindVertexBuffers(g_cmd, 0, 1, &c.buf, &zero);
			vkCmdDraw(g_cmd, c.count, 1, 0, 0);
			drawn++; verts += c.count;
		}
	}
	g_pfFarDraws += drawn; g_pfFarVerts += verts;
	g_farVisible.clear();
}

// ---- FMV: one texture the player fills with each decoded frame (RGB8 on the game's side) ----
static Staging g_fmvStage[FRAMES];

TextureID GR_PS5_FMVCreate()
{
	if (!g_fmvTex) {
		g_tex.emplace_back();
		g_fmvTex = (TextureID)(g_tex.size() - 1);
		g_tex[g_fmvTex].alive = true;
	}
	return g_fmvTex;
}

void GR_PS5_FMVUpload(TextureID id, int w, int h, const unsigned char* rgb)
{
	if (id >= g_tex.size() || !g_tex[id].alive || w <= 0 || h <= 0) return;
	Tex& t = g_tex[id];
	openFrame();
	endRendering();
	if (t.img.w != (uint32_t)w || t.img.h != (uint32_t)h) {
		// a new picture size: a new image (the old one may still be in flight)
		vkDeviceWaitIdle(H->device);
		if (t.img.image) {
			vkFreeDescriptorSets(H->device, g_descPool, 1, &t.set);
			vkDestroyImageView(H->device, t.img.view, nullptr);
			vkDestroyImage(H->device, t.img.image, nullptr);
			vkFreeMemory(H->device, t.img.memory, nullptr);
		}
		t.img = Image();
		createImage(t.img, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
		t.set = makeTexSet(t.img.view, g_samplerLinear);
	}
	Staging& st = g_fmvStage[g_slot];
	const VkDeviceSize need = (VkDeviceSize)w * h * 4;
	if (st.size < need) {
		vkDeviceWaitIdle(H->device);
		if (st.buffer) { vkDestroyBuffer(H->device, st.buffer, nullptr); vkFreeMemory(H->device, st.memory, nullptr); }
		createStaging(st, need);
	}
	unsigned char* d = (unsigned char*)st.mapped;
	for (size_t i = 0, n = (size_t)w * h; i < n; i++, rgb += 3, d += 4) { d[0] = rgb[0]; d[1] = rgb[1]; d[2] = rgb[2]; d[3] = 255; }
	barrier(g_cmd, t.img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	VkBufferImageCopy c{};
	c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	c.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
	vkCmdCopyBufferToImage(g_cmd, st.buffer, t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
	barrier(g_cmd, t.img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

// the OpenGL calls the FMV player still made (it is patched at build time now)
extern "C" {
static void red2_noop() {}
void* glad_glReadPixels = (void*)red2_noop;
void* glad_glActiveTexture = (void*)red2_noop;
void* glad_glBindTexture = (void*)red2_noop;
void* glad_glGenTextures = (void*)red2_noop;
void* glad_glTexImage2D = (void*)red2_noop;
void* glad_glTexParameteri = (void*)red2_noop;
void* glad_glUniform1i = (void*)red2_noop;
}
