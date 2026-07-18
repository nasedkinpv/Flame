#include "metal_bridge/DK2BridgeProtocol.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

template <typename T>
void append(std::vector<uint8_t> &bytes, const T &value) {
    const auto *start = reinterpret_cast<const uint8_t *>(&value);
    bytes.insert(bytes.end(), start, start + sizeof(T));
}

void appendBytes(std::vector<uint8_t> &bytes, const void *data, size_t size) {
    const auto *start = static_cast<const uint8_t *>(data);
    bytes.insert(bytes.end(), start, start + size);
}

[[noreturn]] void die(const char *operation) {
    std::fprintf(stderr, "%s: %s\n", operation, std::strerror(errno));
    std::exit(1);
}

} // namespace

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s BRIDGE_FILE\n", argv[0]);
        return 2;
    }

    const std::string path = argv[1];
    const int descriptor = open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (descriptor < 0) die("open");
    if (ftruncate(descriptor, DK2M_FILE_SIZE) != 0) die("ftruncate");
    void *mapping = mmap(nullptr, DK2M_FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    if (mapping == MAP_FAILED) die("mmap");

    auto *header = static_cast<DK2MFileHeader *>(mapping);
    if (header->magic != DK2M_MAGIC || header->version != DK2M_VERSION) {
        std::memset(mapping, 0, DK2M_FILE_SIZE);
        header->magic = DK2M_MAGIC;
        header->version = DK2M_VERSION;
        header->header_size = sizeof(DK2MFileHeader);
        header->file_size = DK2M_FILE_SIZE;
        header->latest_slot = DK2M_NO_SLOT;
    }

    constexpr uint32_t width = 640;
    constexpr uint32_t height = 480;
    std::vector<uint8_t> commands;

    DK2MClearCommand clear = {};
    clear.header = {DK2M_COMMAND_CLEAR, 0, sizeof(clear)};
    clear.red = 0.025f;
    clear.green = 0.04f;
    clear.blue = 0.055f;
    clear.alpha = 1.0f;
    append(commands, clear);

    const DK2MVertex1C vertices[] = {
        {130.0f, 100.0f, 0.5f, 1.0f, 0xFFE2B84Bu, 0.0f, 0.0f},
        {510.0f, 100.0f, 0.5f, 1.0f, 0xFFB06FE8u, 1.0f, 0.0f},
        {510.0f, 380.0f, 0.5f, 1.0f, 0xFF56318Du, 1.0f, 1.0f},
        {130.0f, 380.0f, 0.5f, 1.0f, 0xFF7A481Eu, 0.0f, 1.0f},
    };
    const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    DK2MDrawIndexedCommand draw = {};
    draw.header.type = DK2M_COMMAND_DRAW_INDEXED;
    draw.header.size = sizeof(draw) + sizeof(vertices) + sizeof(indices);
    draw.fvf = DK2M_FVF_VERTEX1C;
    draw.vertex_count = 4;
    draw.index_count = 6;
    draw.flags = 0x1C;
    append(commands, draw);
    appendBytes(commands, vertices, sizeof(vertices));
    appendBytes(commands, indices, sizeof(indices));

    const uint32_t previousSlot = __atomic_load_n(&header->latest_slot, __ATOMIC_ACQUIRE);
    const uint32_t slotIndex = previousSlot == DK2M_NO_SLOT ? 0 : (previousSlot + 1) % DK2M_SLOT_COUNT;
    DK2MFrameSlot *slot = &header->slots[slotIndex];
    uint32_t sequence = __atomic_load_n(&slot->sequence, __ATOMIC_RELAXED);
    if ((sequence & 1u) != 0) ++sequence;
    __atomic_store_n(&slot->sequence, sequence + 1, __ATOMIC_RELEASE);

    std::memcpy(static_cast<uint8_t *>(mapping) + DK2M_SLOT_OFFSET(slotIndex), commands.data(), commands.size());
    slot->frame_number = 1;
    slot->byte_count = static_cast<uint32_t>(commands.size());
    slot->command_count = 2;
    slot->width = width;
    slot->height = height;
    header->width = width;
    header->height = height;
    header->producer_pid = static_cast<uint32_t>(getpid());

    __atomic_store_n(&slot->sequence, sequence + 2, __ATOMIC_RELEASE);
    __atomic_store_n(&header->latest_slot, slotIndex, __ATOMIC_RELEASE);
    __atomic_store_n(&header->latest_frame, 1, __ATOMIC_RELEASE);

    std::printf("BRIDGE WRITE PASS: frame=1 bytes=%zu slot=%u\n", commands.size(), slotIndex);
    munmap(mapping, DK2M_FILE_SIZE);
    close(descriptor);
    return 0;
}
