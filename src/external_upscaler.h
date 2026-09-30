// SPDX-License-Identifier: BSD-2-Clause
//
// Gamescope external-upscaler plugin ABI. Plain C, struct-of-function-pointers, one exported
// entry point. A plugin runs the "shrink the game, upscale before present" step on any device --
// an NPU, a second GPU, or the CPU -- instead of gamescope's own FSR1/NIS compute shaders.
//
// Versioning: the entry point takes the ABI version the host was built against and returns NULL
// if it cannot serve that version. Every struct below carries its own `size` as its first field,
// written by the allocator (host for *_desc_t it fills in, plugin for *_caps_t it fills in) before
// the call; a struct grows only by appending fields, and either side stops reading past the size
// it received. This is the forward-compat mechanism: a v1 host and a v2 plugin (or vice versa)
// interoperate on the v1 subset without a version bump to the whole ABI.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAMESCOPE_EXTERNAL_UPSCALER_ABI_VERSION 1

// ---------------------------------------------------------------------------------------------
// 1. Device identity
// ---------------------------------------------------------------------------------------------

typedef enum gs_upscaler_device_kind_t
{
	GS_UPSCALER_DEVICE_UNKNOWN = 0,
	GS_UPSCALER_DEVICE_CPU     = 1,
	GS_UPSCALER_DEVICE_GPU     = 2,   // DRM render node, dev_node below is meaningful
	GS_UPSCALER_DEVICE_NPU     = 3,   // e.g. /dev/accel/accelN; dev_node is that node's dev_t when it has one
} gs_upscaler_device_kind_t;

typedef struct gs_upscaler_device_info_t
{
	uint32_t size; // sizeof(gs_upscaler_device_info_t) as filled in by the plugin
	gs_upscaler_device_kind_t kind;
	// The device's DRM render-node dev_t (major/minor of e.g. /dev/dri/renderD128), 0 if none
	// (CPU, or an accel device with no DRM node). Gamescope uses this to decide whether a buffer
	// is already local to the plugin's device (same dev_t as its own render GPU -> zero-copy
	// possible without an import) or must cross an importer (dma-buf import/export, or a copy).
	dev_t dev_node;
	// Free-form, e.g. "xdna:strix-halo", "vulkan:AMD Radeon 890M", "cpu". Diagnostics/logging only,
	// never parsed for behavior.
	char name[128];
} gs_upscaler_device_info_t;

// ---------------------------------------------------------------------------------------------
// 2. Negotiation
// ---------------------------------------------------------------------------------------------

typedef enum gs_upscaler_colorspace_t
{
	GS_UPSCALER_COLORSPACE_SRGB        = 0,
	GS_UPSCALER_COLORSPACE_SCRGB       = 1, // linear, extended range
	GS_UPSCALER_COLORSPACE_HDR10_PQ    = 2,
	GS_UPSCALER_COLORSPACE_BT2020_HLG  = 3,
} gs_upscaler_colorspace_t;

// What gamescope proposes for one negotiation. Immutable once passed to negotiate(); a size
// change re-negotiates with a new struct (see reconfigure below), it never mutates this one.
typedef struct gs_upscaler_negotiate_desc_t
{
	uint32_t size; // sizeof(gs_upscaler_negotiate_desc_t), set by gamescope
	uint32_t in_w, in_h;   // the game's render (layer-0) resolution
	uint32_t out_w, out_h; // the panel/output resolution the result must land at
	gs_upscaler_colorspace_t colorspace;
	bool hdr;              // true if the layer carries HDR metadata; a v1 plugin that only
	                        // handles SDR must decline rather than silently clip/quantize
	float sharpness;        // 0 (max sharpen) .. 20 (min), gamescope's existing FSR1 scale
} gs_upscaler_negotiate_desc_t;

// One input format a plugin is willing to accept, in order of preference (index 0 = most
// preferred). "Converted plane" formats stand in for what gamescope's readback shaders already
// produce today (padded BGRA8, padded Y8 + NV12 chroma, ...) as named, portable options instead
// of each plugin re-deriving its own padding/plane layout; RAW is the layer's own swapchain
// format, untouched.
typedef enum gs_upscaler_plane_kind_t
{
	GS_UPSCALER_PLANE_RAW                = 0, // the layer's own buffer, no gamescope-side conversion
	GS_UPSCALER_PLANE_BGRA8               = 1, // gamescope-converted, gamma-encoded BGRA8, optional padding
	GS_UPSCALER_PLANE_Y8_NV12_OUT         = 2, // gamescope-converted padded Y8 in; plugin writes NV12 Y, gamescope fills chroma
} gs_upscaler_plane_kind_t;

typedef struct gs_upscaler_format_t
{
	uint32_t size;
	gs_upscaler_plane_kind_t plane;
	uint32_t drm_fourcc;          // DRM_FORMAT_* the plugin wants that plane delivered as
	uint64_t drm_modifier;        // DRM_FORMAT_MOD_LINEAR is always an acceptable answer here;
	                                // a plugin MAY additionally list device-specific tiled modifiers
	uint32_t pad_x, pad_y;         // required left/top edge-replicated padding, in texels, 0 if none
	uint32_t align_w, align_h;     // required width/height alignment (rows/cols), 1 if none
	// A kernel compiled for a fixed tile geometry may need a larger buffer than pad_x/pad_y (the
	// edge-replication margin) implies. 0 means no separate padded buffer: use in_w x in_h and the
	// negotiated out_w x out_h.
	uint32_t padded_w, padded_h;         // full padded INPUT plane size gamescope must allocate, or 0
	uint32_t out_padded_w, out_padded_h; // full padded OUTPUT plane size gamescope must allocate, or 0
} gs_upscaler_format_t;

// Plugin's answer, written in place by negotiate(). accepted=false stops gamescope with the sizes
// in its log: a selected plugin is never replaced by another upscaler. A plugin that cannot reach
// out_w x out_h exactly may accept with a smaller scale; gamescope stretches its output the rest of
// the way to the shown size.
typedef struct gs_upscaler_negotiate_result_t
{
	uint32_t size;
	bool accepted;
	uint32_t scale_num, scale_den; // output = input * scale_num / scale_den (FSR1 today: N/1)
	// Formats this plugin accepts for THIS negotiation, most preferred first; gamescope picks the
	// first entry it can itself produce (RAW is always producible; a BGRA8/Y8 entry requires the
	// matching readback shader gamescope already ships) and rejects the plugin if none match.
	uint32_t format_count;
	gs_upscaler_format_t formats[8];
	// True if the plugin can write its output directly into a buffer gamescope hands it (import
	// gamescope's dmabuf) -- otherwise gamescope must import the plugin's own output allocation
	// after submit() instead of pre-allocating one for it.
	bool writes_into_host_buffer;
} gs_upscaler_negotiate_result_t;

// ---------------------------------------------------------------------------------------------
// 3. Async submit
// ---------------------------------------------------------------------------------------------

// One dma-buf plane as exported by gamescope (or, for the output when writes_into_host_buffer is
// false, filled in by the plugin after create/reconfigure so gamescope knows what to import).
typedef struct gs_upscaler_dmabuf_t
{
	uint32_t size;
	int fd;                 // caller-owned; the plugin/host borrows it only for the duration noted
	                          // per-call below, never closes it
	uint32_t width, height;  // padded dimensions this fd actually carries
	uint32_t drm_fourcc;
	uint64_t drm_modifier;
	uint32_t offset, stride; // plane 0 only -- v1 is single-plane formats (RAW multi-plane, e.g.
	                          // a native NV12 swapchain, is out of scope until a plugin needs it)
} gs_upscaler_dmabuf_t;

// submit() is asynchronous and non-blocking: it must return once the work is enqueued, not once
// it finishes. in_fence, if >= 0, is a sync_file fd gamescope signals when `in` is safe to read
// (borrowed for the call only; the plugin dup()s it if it needs to keep it). The plugin returns a
// sync_file fd (caller/gamescope takes ownership, must close it) gamescope waits on before
// sampling `out`; returning -1 means the plugin already waited internally and `out` is ready the
// moment submit() returns.
typedef struct gs_upscaler_submit_t
{
	uint32_t size;
	gs_upscaler_dmabuf_t in;
	gs_upscaler_dmabuf_t out;
	int in_fence_fd;   // -1 = no fence, `in` is already ready
} gs_upscaler_submit_t;

// ---------------------------------------------------------------------------------------------
// 4. Lifecycle
// ---------------------------------------------------------------------------------------------

typedef struct gs_upscaler_instance gs_upscaler_instance; // opaque, plugin-defined

typedef struct gs_upscaler_create_desc_t
{
	uint32_t size;
	// Plugin-specific configuration string, e.g. an env var's value or a CLI suffix after
	// "external:/path/to/plugin.so:"; NULL/empty is valid, the plugin picks its own default
	// (device index, schedule path, ...).
	const char *config;
} gs_upscaler_create_desc_t;

typedef struct gs_upscaler_api_t
{
	uint32_t size;          // sizeof(gs_upscaler_api_t) as filled in by the plugin at get_api() time
	uint32_t abi_version;   // GAMESCOPE_EXTERNAL_UPSCALER_ABI_VERSION this plugin was built against

	// Lifecycle. create() does device/library setup (may fail, e.g. no such device index);
	// destroy() releases everything, including any in-flight submit -- the caller must have
	// waited on the last returned fence first.
	gs_upscaler_instance *( *create )( const gs_upscaler_create_desc_t *desc );
	void ( *destroy )( gs_upscaler_instance *inst );

	void ( *get_device_info )( gs_upscaler_instance *inst, gs_upscaler_device_info_t *out_info );

	// Negotiate a size/format. Callable again at any time with a new desc (a layer resize, e.g.
	// the game changing resolution) -- no restart, no destroy/create cycle required. The plugin
	// must free any size-dependent resources from a previous negotiate() before returning.
	void ( *negotiate )( gs_upscaler_instance *inst, const gs_upscaler_negotiate_desc_t *desc,
	                      gs_upscaler_negotiate_result_t *out_result );

	// Valid only after an accepted negotiate(); async, see gs_upscaler_submit_t above. Returns a
	// sync_file fd or -1. -2 signals a hard failure (device lost, etc.) and stops gamescope.
	int ( *submit )( gs_upscaler_instance *inst, const gs_upscaler_submit_t *submit );
} gs_upscaler_api_t;

// The one exported symbol. host_abi_version is GAMESCOPE_EXTERNAL_UPSCALER_ABI_VERSION from the
// gamescope build that loaded this plugin; a plugin built against a newer major ABI that cannot
// serve an older host returns NULL. dlsym name: "gamescope_external_upscaler_get_api".
typedef const gs_upscaler_api_t *( *gs_upscaler_get_api_fn )( uint32_t host_abi_version );

#ifdef __cplusplus
}
#endif
