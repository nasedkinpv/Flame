#ifndef DK2_METAL_BRIDGE_PROTOCOL_H
#define DK2_METAL_BRIDGE_PROTOCOL_H

#include <stdint.h>

#define DK2M_MAGIC 0x4D324B44u
#define DK2M_VERSION 15u
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
// DK2MDrawIndexedCommand.flags bit: this draw is a shadow decal. v15: set
// GAME-SIDE from the engine's own semantic (shadow SceneObject2E, anim mode
// >= 2000 / f2C_ >= 0x7D0), carried through the ToDraw grouping to the
// submission - never guessed from pages or UVs. The game's own flag word
// only ever carries 0x1C, so a high bit is safe. The host uses it to
// redirect sampling to its shadow twin of the bound page.
#define DK2M_DRAW_INDEXED_SHADOW_DECAL (1u << 16)
#define DK2M_MAX_LIGHTS_PER_DRAW 24u

enum DK2MCommandType {
    DK2M_COMMAND_CLEAR = 1,
    DK2M_COMMAND_DRAW_INDEXED = 2,
    DK2M_COMMAND_TEXTURE_UPDATE = 3,
    DK2M_COMMAND_SET_TEXTURE = 4,
    DK2M_COMMAND_RENDER_STATE = 5,
    DK2M_COMMAND_TEXTURE_STAGE_STATE = 6,
    DK2M_COMMAND_TEXTURE_UPDATE_RECT = 7,
    // World-space mesh pipeline (introduced in v9): the game registers object-
    // space meshes once, then per frame sends camera + lights + per-instance
    // world transforms, and the Metal vertex shader does transform+lighting -
    // replacing the original engine's per-vertex CPU pipeline.
    DK2M_COMMAND_MESH_REGISTER = 8,
    DK2M_COMMAND_CAMERA_SET = 9,
    DK2M_COMMAND_LIGHTS_SET = 10,
    DK2M_COMMAND_DRAW_MESH = 11,
    // Compatibility variant for emitters whose whole geometry still changes:
    // world-space vertices travel every frame (no registry), while projection
    // and lighting remain on the GPU. World transform is implicitly identity.
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
    // subpixel coordinate space. Immediate-mode as of the shadow redesign:
    // the target region is resolved AT CAPTURE TIME on the game side and the
    // command is emitted into the frame whose bake produced it - the host
    // must never re-resolve or retain producer-side identity. Application
    // order: masks apply AFTER this frame's TEXTURE_UPDATE/_RECT commands
    // (post-pass), and a mask's content persists in its target region until
    // a later mask overwrites it (low-detail blob shadows are baked once and
    // then reused for many frames, so per-frame clearing would drop them).
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
    // Retained animated topology with only the interpolated float3 positions
    // supplied per frame. Normals, UVs and indices come from MESH_REGISTER.
    DK2M_COMMAND_DRAW_MESH_DEFORMED = 17,
    // Native scene mirror (Phase 1, LOG-ONLY / observational): the guest
    // registers every static scene object so the host can build a mirror
    // registry for future native culling. signature/vertex_count are
    // PRE-COMPUTED on the guest (describe stays guest-side -- the host
    // cannot read x86 vertex/index buffers without shared-memory, which is
    // out of scope). Versioned by scene_epoch; SCENE_RESET bumps it on
    // level-load / save-load and the host drops its registry. Nothing is
    // consumed in Phase 1 -- the guest does not skip its draw-walk.
    DK2M_COMMAND_SCENE_REGISTER = 18,
    DK2M_COMMAND_SCENE_RESET = 19,
};

struct DK2MPageAtlasMapCommand {
    uint32_t textureId;
    uint32_t generation;  // page generation these placements describe (v15)
    uint16_t x, y, w, h;
    char name[64];  // NUL-terminated, truncated if longer
};

enum DK2MDrawMeshFlags {
    DK2M_DRAW_MESH_LIT = 1u << 0,          // apply point-light accumulation
    DK2M_DRAW_MESH_ALPHA_BLEND = 1u << 1,  // SRCALPHA/INVSRCALPHA blend
    DK2M_DRAW_MESH_ADDITIVE = 1u << 2,     // ONE/ONE additive blend
    DK2M_DRAW_MESH_ALPHA_TEST = 1u << 3,   // discard texels below the ref, rest opaque
    DK2M_DRAW_MESH_MULTIPLY = 1u << 4,     // ZERO/INVSRCCOLOR blend
    DK2M_DRAW_MESH_Z_ENABLE = 1u << 5,
    DK2M_DRAW_MESH_Z_WRITE = 1u << 6,
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
    uint32_t cursor_scale_bits;
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
    uint32_t generation;  // page generation of these pixels (v15; 0 = non-repacking texture)
    uint32_t width;
    uint32_t height;
    uint32_t row_pitch;
    uint32_t data_size;
} DK2MTextureUpdateCommand;

typedef struct DK2MTextureUpdateRectCommand {
    DK2MCommandHeader header;
    uint32_t texture_id;
    uint32_t generation;  // page generation of these pixels (v15; 0 = non-repacking texture)
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
    uint32_t generation;  // the NEW generation this reset opens (v15)
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
    // Native scene-mirror cull inputs (Phase 2, LOG-ONLY). The host reproduces
    // the guest's per-object frustum-sphere cull (Vec3f_static_sub_575D70)
    // bit-for-bit: for each mirrored object it forms rel = center - cull_cam_pos,
    // camPos = cull_cam_rot^T * rel (out_i = sum_c rot[c][i]*rel_c, matching
    // Mat3x3f::sub_594E10 / combineRows), then dots camPos against the four
    // camera-space frustum-side plane normals cull_plane[0..3]. These are the
    // guest globals g_camState.v3f (cam_pos), g_camState.m row-major 3x3
    // (cam_rot), and g_vec_760B70/_760B38/_760B18/_760B28 (planes A,B,C,D).
    // Appended at the end so the existing fixed 24-float view_proj+depth read is
    // unchanged; populated every mesh frame, consumed only by the log-only
    // mirror -- never affects rendering.
    float cull_plane[4][3];  // A=760B70, B=760B38, C=760B18, D=760B28
    float cull_cam_pos[3];   // g_camState.v3f (camera world position)
    float cull_cam_rot[9];   // g_camState.m.m[3][3], row-major
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
    float uv_scale_u, uv_scale_v, uv_offset_u, uv_offset_v;
    uint32_t light_count;
    uint16_t light_indices[DK2M_MAX_LIGHTS_PER_DRAW];
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
    uint32_t light_count;
    uint16_t light_indices[DK2M_MAX_LIGHTS_PER_DRAW];
} DK2MDrawMeshInlineCommand;

// Per-frame deformation of a retained mesh. Payload is vertex_count packed
// float3 positions in the same dense vertex order as MESH_REGISTER.
typedef struct DK2MDrawMeshDeformedCommand {
    DK2MCommandHeader header;
    uint32_t mesh_id;
    uint32_t texture_id;
    uint32_t flags;
    uint32_t tint;
    uint32_t vertex_count;
    float ambient_r, ambient_g, ambient_b;
    float world[12];
    float uv_scale_u, uv_scale_v, uv_offset_u, uv_offset_v;
    uint32_t light_count;
    uint16_t light_indices[DK2M_MAX_LIGHTS_PER_DRAW];
} DK2MDrawMeshDeformedCommand;

typedef struct DK2MShadowTriangle {
    int32_t x0, y0;
    int32_t x1, y1;
    int32_t x2, y2;
} DK2MShadowTriangle;

typedef struct DK2MShadowMaskCommand {
    DK2MCommandHeader header;
    uint32_t texture_id;
    uint32_t generation;  // page generation this mask belongs to (v15)
    uint32_t target_x;
    uint32_t target_y;
    uint32_t target_width;
    uint32_t target_height;
    uint32_t triangle_count;
    uint32_t mode;  // DK2MShadowMaskMode
} DK2MShadowMaskCommand;

// One static scene object in the native mirror. object_id is the guest-side
// scene pointer (stable within a session for static objects; reused only
// across epoch bumps). mesh_id matches MESH_REGISTER. world is row-major 3x4.
// center/radius is the object's bounding sphere (vec / f20 from the guest).
typedef struct DK2MSceneRegisterCommand {
    DK2MCommandHeader header;
    uint32_t scene_epoch;
    uint32_t object_id;
    uint32_t mesh_id;
    uint64_t signature;       // describe content hash (full 64-bit; mirrors use it for invalidation)
    uint32_t vertex_count;
    uint32_t material_flags;  // DK2MDrawMeshFlags-equivalent
    float world[12];
    float center[3];
    float radius;
    // Guest's own frustum-sphere cull verdict for this object (Phase 2), stamped
    // by recomputing Vec3f_static_sub_575D70 at register time from the SAME world
    // bounds + live camera the host receives. bit0 = visible, bit1 = fullyInside.
    // The host diffs its independent recompute against this to prove the ported
    // math + camera-state reconstruction agree bit-for-bit. LOG-ONLY.
    uint32_t guest_cull;
} DK2MSceneRegisterCommand;

// guest_cull bit meanings. bit0/bit1 are the Phase 2 log-only cull verdict.
// bit2 is the Phase 3 signal that the guest was launched with
// flametal:native_scene_mirror_consume=true (only ever set when
// native_scene_mirror is ALSO on): it tells the host it MAY skip a draw whose
// OWN independently-recomputed cull verdict says "culled". Reusing a spare bit
// keeps the struct size (104) unchanged, so no host/guest re-verify is needed.
#define DK2M_GUEST_CULL_VISIBLE      0x1u  // bit0: guest frustum-sphere test passed
#define DK2M_GUEST_CULL_FULLY_INSIDE 0x2u  // bit1: sphere fully inside the frustum
#define DK2M_GUEST_CULL_CONSUME      0x4u  // bit2: Phase 3 host consumption requested

// Bumps the scene epoch (level-load / save-load / new game). The host drops
// its entire mirror registry on receipt.
typedef struct DK2MSceneResetCommand {
    DK2MCommandHeader header;
    uint32_t scene_epoch;
} DK2MSceneResetCommand;
#pragma pack(pop)

#define DK2M_FILE_SIZE ((uint32_t)(sizeof(DK2MFileHeader) + DK2M_SLOT_COUNT * DK2M_SLOT_CAPACITY))
#define DK2M_SLOT_OFFSET(slot) ((uint32_t)(sizeof(DK2MFileHeader) + (slot) * DK2M_SLOT_CAPACITY))

#if defined(__cplusplus)
static_assert(sizeof(DK2MFrameSlot) == 40, "bridge slot layout changed");
static_assert(sizeof(DK2MInputEvent) == 8, "bridge input event layout changed");
static_assert(sizeof(DK2MInputState) == 596, "bridge input state layout changed");
static_assert(sizeof(DK2MFileHeader) == 764, "bridge header layout changed");
static_assert(sizeof(DK2MCommandHeader) == 8, "bridge command layout changed");
static_assert(sizeof(DK2MClearCommand) == 24, "bridge clear layout changed");
static_assert(sizeof(DK2MVertex1C) == 28, "DK2 Vertex1C layout changed");
static_assert(sizeof(DK2MVertex2C) == 44, "DK2 Vertex2C layout changed");
static_assert(sizeof(DK2MDrawIndexedCommand) == 24, "bridge draw layout changed");
static_assert(sizeof(DK2MTextureUpdateCommand) == 32, "bridge texture update layout changed");
static_assert(sizeof(DK2MTextureUpdateRectCommand) == 40,
              "bridge texture rect update layout changed");
static_assert(sizeof(DK2MSetTextureCommand) == 16, "bridge texture binding layout changed");
static_assert(sizeof(DK2MRenderStateCommand) == 16, "bridge render state layout changed");
static_assert(sizeof(DK2MTextureStageStateCommand) == 20,
              "bridge texture stage state layout changed");
static_assert(sizeof(DK2MShadowTriangle) == 24, "bridge shadow triangle layout changed");
static_assert(sizeof(DK2MCameraSetCommand) == 200, "bridge camera set layout changed");
static_assert(sizeof(DK2MShadowMaskCommand) == 40, "bridge shadow mask layout changed");
static_assert(sizeof(DK2MSceneRegisterCommand) == 104, "bridge scene register layout changed");
static_assert(sizeof(DK2MSceneResetCommand) == 12, "bridge scene reset layout changed");
#endif

#endif
