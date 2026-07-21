#ifndef DK2_METAL_BRIDGE_PROTOCOL_H
#define DK2_METAL_BRIDGE_PROTOCOL_H

#include <stdint.h>

#define DK2M_MAGIC 0x4D324B44u
#define DK2M_VERSION 12u
#define DK2M_TIMING_QUANTUM_US 8u
#define DK2M_SLOT_COUNT 3u
// A 1600x1200 High-Res frame can introduce 9-12 MiB of 128x128 surfaces while
// menus are warming up. Four MiB starves later textures forever when earlier
// surfaces are also animated, so leave room for a complete hydration frame.
#define DK2M_SLOT_CAPACITY (16u * 1024u * 1024u)
#define DK2M_NO_SLOT 0xFFFFFFFFu
#define DK2M_FVF_VERTEX1C 0x144u
#define DK2M_FVF_VERTEX2C 0x344u
#define DK2M_OVERLAY_TEXTURE_ID 0xFFFFFFFEu
#define DK2M_CURSOR_TEXTURE_ID 0xFFFFFFFDu
#define DK2M_INPUT_EVENT_CAPACITY 64u

enum DK2MCommandType {
    DK2M_COMMAND_CLEAR = 1,
    DK2M_COMMAND_DRAW_INDEXED = 2,
    DK2M_COMMAND_TEXTURE_UPDATE = 3,
    DK2M_COMMAND_SET_TEXTURE = 4,
    DK2M_COMMAND_RENDER_STATE = 5,
    DK2M_COMMAND_TEXTURE_STAGE_STATE = 6,
    DK2M_COMMAND_TEXTURE_UPDATE_RECT = 7,
    // World-space mesh pipeline (protocol v9): the game registers object-
    // space meshes once, then per frame sends camera + lights + per-instance
    // world transforms, and the Metal vertex shader does transform+lighting -
    // replacing the original engine's per-vertex CPU pipeline.
    DK2M_COMMAND_MESH_REGISTER = 8,
    DK2M_COMMAND_CAMERA_SET = 9,
    DK2M_COMMAND_LIGHTS_SET = 10,
    DK2M_COMMAND_DRAW_MESH = 11,
    // Inline variant for deformed/animated geometry: world-space vertices
    // travel with the command every frame (no registry), the GPU still does
    // projection + lighting. World transform is implicitly identity.
    DK2M_COMMAND_DRAW_MESH_INLINE = 12,
    // Named-atlas map (HD resource pack): reports that the rect [x,y,w,h]
    // of bridge texture `textureId` was composited from the resource named
    // `name` (asciiz, WAD-style, mip suffix stripped). The host uses these
    // to assemble a scaled HD replacement page from loose PNGs in the
    // user's resource pack; rects for ids it has no HD art for fall back
    // to scaling the original pixels. Sent once per composited rect, may
    // repeat when a page is re-composited.
    DK2M_COMMAND_PAGE_ATLAS_MAP = 13,
    // Original DK2 shadow silhouette after its per-light projection. Payload
    // is triangle_count DK2MShadowTriangle records in the engine's 256x256
    // subpixel coordinate space. The host rasterizes/downsamples them into
    // the specified region of the original texture atlas before scene draws.
    DK2M_COMMAND_SHADOW_MASK = 14,
    // The game-side owner released this bridge texture id. The host drops
    // both the Metal texture and persistent named-atlas metadata for it.
    DK2M_COMMAND_TEXTURE_RELEASE = 15,
    // Atlas page repack: the engine detached this page's handles and will
    // recomposite it from scratch (SurfHashList2 repack). The host must
    // drop every PAGE_ATLAS_MAP rect it holds for `texture_id`; placements
    // that follow describe the new layout. Without this, historical rects
    // keep getting composed into reused pages (stale sprites glued into
    // unrelated textures).
    DK2M_COMMAND_PAGE_ATLAS_RESET = 16,
};

struct DK2MPageAtlasMapCommand {
    uint32_t textureId;
    uint16_t x, y, w, h;
    char name[64];  // NUL-terminated, truncated if longer
};

enum DK2MDrawMeshFlags {
    DK2M_DRAW_MESH_LIT = 1u << 0,          // apply point-light accumulation
    DK2M_DRAW_MESH_ALPHA_BLEND = 1u << 1,  // SRCALPHA/INVSRCALPHA blend
    DK2M_DRAW_MESH_ADDITIVE = 1u << 2,     // ONE/ONE additive blend
    DK2M_DRAW_MESH_ALPHA_TEST = 1u << 3,   // discard texels below the ref, rest opaque
};

enum DK2MInputFlags {
    DK2M_INPUT_ACTIVE = 1u << 0,
    DK2M_INPUT_CURSOR_VALID = 1u << 1,
    // Host shadow pipeline is available and the live setting is enabled.
    // The game keeps the original CPU rasterizer whenever this bit is clear.
    DK2M_INPUT_METAL_SHADOWS = 1u << 2,
};

enum DK2MShadowMaskMode {
    DK2M_SHADOW_MASK_ALPHA = 0,
    DK2M_SHADOW_MASK_GRAYSCALE = 1,
};

enum DK2MInputEventType {
    DK2M_INPUT_EVENT_BUTTON = 1,
    DK2M_INPUT_EVENT_KEY = 2,
    DK2M_INPUT_EVENT_CHAR = 3,
};

#pragma pack(push, 4)
typedef struct DK2MFrameSlot {
    volatile uint32_t sequence;
    uint32_t frame_number;
    uint32_t byte_count;
    uint32_t command_count;
    uint32_t width;
    uint32_t height;
    uint32_t game_timings[2];
    uint32_t producer_timings[2];
} DK2MFrameSlot;

typedef struct DK2MInputEvent {
    uint16_t type;
    uint16_t code;
    int32_t value;
} DK2MInputEvent;

typedef struct DK2MInputState {
    volatile uint32_t sequence;
    uint32_t flags;
    float cursor_x;
    float cursor_y;
    uint32_t relative_x;
    uint32_t relative_y;
    uint32_t wheel_x;
    uint32_t wheel_y;
    uint32_t buttons;
    uint32_t host_pid;
    uint8_t keys[32];
    uint32_t event_write;
    DK2MInputEvent events[DK2M_INPUT_EVENT_CAPACITY];
    uint32_t heartbeat;
} DK2MInputState;

typedef struct DK2MFileHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t file_size;
    volatile uint32_t latest_slot;
    volatile uint32_t latest_frame;
    volatile uint32_t consumer_frame;
    volatile uint32_t input_ack_sequence;
    uint32_t width;
    uint32_t height;
    uint32_t producer_pid;
    volatile uint32_t consumer_session;
    DK2MFrameSlot slots[DK2M_SLOT_COUNT];
    DK2MInputState input;
} DK2MFileHeader;

typedef struct DK2MCommandHeader {
    uint16_t type;
    uint16_t reserved;
    uint32_t size;
} DK2MCommandHeader;

typedef struct DK2MClearCommand {
    DK2MCommandHeader header;
    float red;
    float green;
    float blue;
    float alpha;
} DK2MClearCommand;

typedef struct DK2MVertex1C {
    float x;
    float y;
    float z;
    float rhw;
    uint32_t diffuse;
    float u;
    float v;
} DK2MVertex1C;

typedef struct DK2MVertex2C {
    float x;
    float y;
    float z;
    float rhw;
    uint32_t diffuse;
    float tex_coord[3][2];
} DK2MVertex2C;

typedef struct DK2MDrawIndexedCommand {
    DK2MCommandHeader header;
    uint32_t fvf;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t flags;
} DK2MDrawIndexedCommand;

typedef struct DK2MTextureUpdateCommand {
    DK2MCommandHeader header;
    uint32_t texture_id;
    uint32_t width;
    uint32_t height;
    uint32_t row_pitch;
    uint32_t data_size;
} DK2MTextureUpdateCommand;

typedef struct DK2MTextureUpdateRectCommand {
    DK2MCommandHeader header;
    uint32_t texture_id;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t row_pitch;
    uint32_t data_size;
} DK2MTextureUpdateRectCommand;

typedef struct DK2MSetTextureCommand {
    DK2MCommandHeader header;
    uint32_t stage;
    uint32_t texture_id;
} DK2MSetTextureCommand;

typedef struct DK2MTextureReleaseCommand {
    DK2MCommandHeader header;
    uint32_t texture_id;
} DK2MTextureReleaseCommand;

typedef struct DK2MPageAtlasResetCommand {
    DK2MCommandHeader header;
    uint32_t texture_id;
} DK2MPageAtlasResetCommand;

typedef struct DK2MRenderStateCommand {
    DK2MCommandHeader header;
    uint32_t state;
    uint32_t value;
} DK2MRenderStateCommand;

typedef struct DK2MTextureStageStateCommand {
    DK2MCommandHeader header;
    uint32_t stage;
    uint32_t state;
    uint32_t value;
} DK2MTextureStageStateCommand;

// Object-space mesh vertex. Matches the layout the DK2 mesh emitters consume
// (position, normal, UV, per-vertex base colour) so producers can copy
// straight out of engine data.
typedef struct DK2MMeshVertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    uint32_t base_color;  // ARGB, pre-lighting per-vertex colour
} DK2MMeshVertex;

// One-time mesh upload; payload = vertex_count DK2MMeshVertex followed by
// index_count uint16 indices (padded to 4 bytes). The consumer caches the
// blob by mesh_id for the rest of its session.
typedef struct DK2MMeshRegisterCommand {
    DK2MCommandHeader header;
    uint32_t mesh_id;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t flags;
} DK2MMeshRegisterCommand;

// Per-frame camera: world -> clip transform (column-major 4x4) plus the
// engine's piecewise depth mapping (near: z_mul2*z + z_add2; far:
// z_add3 - z_mul3_f/z; capped at depth_cap; switch at far_threshold).
typedef struct DK2MCameraSetCommand {
    DK2MCommandHeader header;
    float view_proj[16];
    float z_mul2, z_add2;
    float z_add3, z_mul3_f;
    float far_threshold, depth_cap;
    float pad0, pad1;
} DK2MCameraSetCommand;

// World-space point light, matching DK2's per-vertex accumulation model
// (see Obj57BCB0): contribution = (dist_sq_limit - d2) * falloff_lut[f(d2)]
// * atten_scale * max(dot(normal, to_light) * facing_scale, 0), clamped.
typedef struct DK2MLight {
    float px, py, pz;
    float r, g, b;
    float dist_sq_limit;
    float atten_scale;
    float facing_scale;
    float pad0, pad1, pad2;
} DK2MLight;

// Per-frame light set; payload = 256 floats of the engine falloff LUT
// (captured from the game) followed by light_count DK2MLight.
typedef struct DK2MLightsSetCommand {
    DK2MCommandHeader header;
    uint32_t light_count;
    float ambient_r, ambient_g, ambient_b;
} DK2MLightsSetCommand;

// One mesh instance: row-major 3x4 world transform (rows of [3x3 rot*scale |
// translation]) plus material inputs. Depth/blend context comes from the
// surrounding RENDER_STATE stream like every other draw.
typedef struct DK2MDrawMeshCommand {
    DK2MCommandHeader header;
    uint32_t mesh_id;
    uint32_t texture_id;
    uint32_t flags;   // DK2MDrawMeshFlags
    uint32_t tint;    // ARGB modulated over the lit colour
    float ambient_r, ambient_g, ambient_b;  // additive per-draw ambient
    float world[12];
} DK2MDrawMeshCommand;

// Inline world-space draw; payload = vertex_count DK2MMeshVertex followed by
// index_count uint16 indices (padded to 4 bytes). Used for deformed geometry
// whose world-space positions change every frame.
typedef struct DK2MDrawMeshInlineCommand {
    DK2MCommandHeader header;
    uint32_t texture_id;
    uint32_t flags;   // DK2MDrawMeshFlags
    uint32_t tint;
    uint32_t vertex_count;
    uint32_t index_count;
    float ambient_r, ambient_g, ambient_b;
} DK2MDrawMeshInlineCommand;

typedef struct DK2MShadowTriangle {
    int32_t x0, y0;
    int32_t x1, y1;
    int32_t x2, y2;
} DK2MShadowTriangle;

typedef struct DK2MShadowMaskCommand {
    DK2MCommandHeader header;
    uint32_t texture_id;
    uint32_t target_x;
    uint32_t target_y;
    uint32_t target_width;
    uint32_t target_height;
    uint32_t triangle_count;
    uint32_t mode;  // DK2MShadowMaskMode
} DK2MShadowMaskCommand;
#pragma pack(pop)

#define DK2M_FILE_SIZE ((uint32_t)(sizeof(DK2MFileHeader) + DK2M_SLOT_COUNT * DK2M_SLOT_CAPACITY))
#define DK2M_SLOT_OFFSET(slot) ((uint32_t)(sizeof(DK2MFileHeader) + (slot) * DK2M_SLOT_CAPACITY))

#if defined(__cplusplus)
static_assert(sizeof(DK2MFrameSlot) == 40, "bridge slot layout changed");
static_assert(sizeof(DK2MInputEvent) == 8, "bridge input event layout changed");
static_assert(sizeof(DK2MInputState) == 592, "bridge input state layout changed");
static_assert(sizeof(DK2MFileHeader) == 760, "bridge header layout changed");
static_assert(sizeof(DK2MCommandHeader) == 8, "bridge command layout changed");
static_assert(sizeof(DK2MClearCommand) == 24, "bridge clear layout changed");
static_assert(sizeof(DK2MVertex1C) == 28, "DK2 Vertex1C layout changed");
static_assert(sizeof(DK2MVertex2C) == 44, "DK2 Vertex2C layout changed");
static_assert(sizeof(DK2MDrawIndexedCommand) == 24, "bridge draw layout changed");
static_assert(sizeof(DK2MTextureUpdateCommand) == 28, "bridge texture update layout changed");
static_assert(sizeof(DK2MTextureUpdateRectCommand) == 36,
              "bridge texture rect update layout changed");
static_assert(sizeof(DK2MSetTextureCommand) == 16, "bridge texture binding layout changed");
static_assert(sizeof(DK2MRenderStateCommand) == 16, "bridge render state layout changed");
static_assert(sizeof(DK2MTextureStageStateCommand) == 20,
              "bridge texture stage state layout changed");
static_assert(sizeof(DK2MShadowTriangle) == 24, "bridge shadow triangle layout changed");
static_assert(sizeof(DK2MShadowMaskCommand) == 36, "bridge shadow mask layout changed");
#endif

#endif
