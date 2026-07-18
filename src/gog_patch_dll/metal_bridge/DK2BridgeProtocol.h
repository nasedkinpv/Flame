#ifndef DK2_METAL_BRIDGE_PROTOCOL_H
#define DK2_METAL_BRIDGE_PROTOCOL_H

#include <stdint.h>

#define DK2M_MAGIC 0x4D324B44u
#define DK2M_VERSION 1u
#define DK2M_SLOT_COUNT 3u
#define DK2M_SLOT_CAPACITY (4u * 1024u * 1024u)
#define DK2M_NO_SLOT 0xFFFFFFFFu
#define DK2M_FVF_VERTEX1C 0x144u
#define DK2M_FVF_VERTEX2C 0x344u

enum DK2MCommandType {
    DK2M_COMMAND_CLEAR = 1,
    DK2M_COMMAND_DRAW_INDEXED = 2,
};

#pragma pack(push, 4)
typedef struct DK2MFrameSlot {
    volatile uint32_t sequence;
    uint32_t frame_number;
    uint32_t byte_count;
    uint32_t command_count;
    uint32_t width;
    uint32_t height;
    uint32_t reserved[2];
} DK2MFrameSlot;

typedef struct DK2MFileHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t file_size;
    volatile uint32_t latest_slot;
    volatile uint32_t latest_frame;
    volatile uint32_t consumer_frame;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t producer_pid;
    uint32_t reserved0;
    DK2MFrameSlot slots[DK2M_SLOT_COUNT];
    uint32_t reserved[28];
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

typedef struct DK2MDrawIndexedCommand {
    DK2MCommandHeader header;
    uint32_t fvf;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t flags;
} DK2MDrawIndexedCommand;
#pragma pack(pop)

#define DK2M_FILE_SIZE ((uint32_t)(sizeof(DK2MFileHeader) + DK2M_SLOT_COUNT * DK2M_SLOT_CAPACITY))
#define DK2M_SLOT_OFFSET(slot) ((uint32_t)(sizeof(DK2MFileHeader) + (slot) * DK2M_SLOT_CAPACITY))

#if defined(__cplusplus)
static_assert(sizeof(DK2MFrameSlot) == 32, "bridge slot layout changed");
static_assert(sizeof(DK2MFileHeader) == 256, "bridge header layout changed");
static_assert(sizeof(DK2MCommandHeader) == 8, "bridge command layout changed");
static_assert(sizeof(DK2MClearCommand) == 24, "bridge clear layout changed");
static_assert(sizeof(DK2MVertex1C) == 28, "DK2 Vertex1C layout changed");
static_assert(sizeof(DK2MDrawIndexedCommand) == 24, "bridge draw layout changed");
#endif

#endif
