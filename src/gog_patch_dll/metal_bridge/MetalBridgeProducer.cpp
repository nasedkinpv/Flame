#include <metal_bridge/MetalBridgeProducer.h>
#include <metal_bridge/DK2BridgeProtocol.h>

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gog::metal_bridge {
namespace {

struct TextureCache {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rowPitch = 0;
    uint32_t lastSentFrame = 0;
    bool pending = true;
    bool dirty = false;
    bool sentInCurrentFrame = false;
    std::vector<uint8_t> pixels;
};

class Producer {
public:
    ~Producer() {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }

    void begin(DWORD width, DWORD height) {
        if (!ensureMapped()) return;
        if (active_) finish();

        const uint32_t consumer = InterlockedCompareExchange(asLong(&header_->consumer_frame), 0, 0);
        const uint32_t consumerSession =
            InterlockedCompareExchange(asLong(&header_->consumer_session), 0, 0);
        if (consumerSession != lastConsumerSession_ || consumer < lastConsumerFrame_) {
            for (auto &entry : textures_) entry.second.pending = true;
        }
        lastConsumerSession_ = consumerSession;
        lastConsumerFrame_ = consumer;

        slotIndex_ = previousSlot_ == DK2M_NO_SLOT ? 0 : (previousSlot_ + 1) % DK2M_SLOT_COUNT;
        previousSlot_ = slotIndex_;
        slot_ = &header_->slots[slotIndex_];
        LONG sequence = InterlockedCompareExchange(asLong(&slot_->sequence), 0, 0);
        if ((sequence & 1) != 0) ++sequence;
        sequence_ = static_cast<uint32_t>(sequence + 2);
        InterlockedExchange(asLong(&slot_->sequence), sequence + 1);

        width_ = width;
        height_ = height;
        used_ = 0;
        commandCount_ = 0;
        active_ = true;

        DK2MClearCommand clear = {};
        clear.header.type = DK2M_COMMAND_CLEAR;
        clear.header.size = sizeof(clear);
        clear.red = 0.0f;
        clear.green = 0.0f;
        clear.blue = 0.0f;
        clear.alpha = 1.0f;
        append(&clear, sizeof(clear));
        ++commandCount_;

        for (DWORD stage = 0; stage < 3; ++stage) {
            if (boundTextures_[stage]) emitTexture(stage, boundTextures_[stage]);
        }
        for (const auto &entry : renderStates_) emitRenderState(entry.first, entry.second);
    }

    void draw(DWORD fvf, const void *vertices, DWORD vertexCount,
              const WORD *indices, DWORD indexCount, DWORD flags) {
        if (!active_ || fvf != DK2M_FVF_VERTEX1C || !vertices || !indices) return;
        const uint64_t vertexBytes = static_cast<uint64_t>(vertexCount) * sizeof(DK2MVertex1C);
        const uint64_t indexBytes = static_cast<uint64_t>(indexCount) * sizeof(WORD);
        const uint64_t commandBytes = sizeof(DK2MDrawIndexedCommand) + vertexBytes + indexBytes;
        if (commandBytes > UINT32_MAX || used_ + commandBytes > DK2M_SLOT_CAPACITY) return;

        DK2MDrawIndexedCommand command = {};
        command.header.type = DK2M_COMMAND_DRAW_INDEXED;
        command.header.size = static_cast<uint32_t>(commandBytes);
        command.fvf = fvf;
        command.vertex_count = vertexCount;
        command.index_count = indexCount;
        command.flags = flags;
        append(&command, sizeof(command));
        append(vertices, static_cast<uint32_t>(vertexBytes));
        append(indices, static_cast<uint32_t>(indexBytes));
        ++commandCount_;
    }

    void texture(DWORD stage, DWORD textureId, IDirectDrawSurface4 *surface) {
        if (stage >= 3 || !ensureMapped()) return;
        boundTextures_[stage] = textureId;
        if (textureId) {
            auto found = textures_.find(textureId);
            if (found == textures_.end()) {
                TextureCache cache;
                if (!captureTexture(surface, cache)) return;
                textures_.emplace(textureId, std::move(cache));
            } else if (found->second.dirty) {
                TextureCache updated;
                if (captureTexture(surface, updated)) found->second = std::move(updated);
            }
            if (surface) surfaceTextures_[reinterpret_cast<uintptr_t>(surface)] = textureId;
        }
        if (active_) emitTexture(stage, textureId);
    }

    void textureDirty(IDirectDrawSurface4 *surface) {
        if (!surface) return;
        const auto surfaceEntry = surfaceTextures_.find(reinterpret_cast<uintptr_t>(surface));
        if (surfaceEntry == surfaceTextures_.end()) return;
        const auto texture = textures_.find(surfaceEntry->second);
        if (texture != textures_.end()) texture->second.dirty = true;
    }

    void renderState(DWORD state, DWORD value) {
        renderStates_[state] = value;
        if (active_) emitRenderState(state, value);
    }

    bool renderState(DWORD state, DWORD *value) const {
        if (!value) return false;
        const auto found = renderStates_.find(state);
        if (found == renderStates_.end()) return false;
        *value = found->second;
        return true;
    }

    void finish() {
        if (!active_) return;
        ++frame_;
        if (frame_ == 0) ++frame_;
        slot_->frame_number = frame_;
        slot_->byte_count = used_;
        slot_->command_count = commandCount_;
        slot_->width = width_;
        slot_->height = height_;
        header_->width = width_;
        header_->height = height_;
        header_->producer_pid = GetCurrentProcessId();
        MemoryBarrier();
        InterlockedExchange(asLong(&slot_->sequence), sequence_);
        InterlockedExchange(asLong(&header_->latest_slot), static_cast<LONG>(slotIndex_));
        InterlockedExchange(asLong(&header_->latest_frame), static_cast<LONG>(frame_));
        for (auto &entry : textures_) {
            TextureCache &texture = entry.second;
            if (texture.sentInCurrentFrame) {
                texture.lastSentFrame = frame_;
                texture.sentInCurrentFrame = false;
            }
        }
        active_ = false;
    }

private:
    static volatile LONG *asLong(volatile uint32_t *value) {
        return reinterpret_cast<volatile LONG *>(value);
    }

    bool ensureMapped() {
        if (header_) return true;
        char path[1024];
        const DWORD length = GetEnvironmentVariableA("DK2_METAL_BRIDGE_FILE", path, sizeof(path));
        if (length == 0 || length >= sizeof(path)) return false;

        file_ = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size = {};
        size.QuadPart = DK2M_FILE_SIZE;
        if (!SetFilePointerEx(file_, size, nullptr, FILE_BEGIN) || !SetEndOfFile(file_)) return false;
        mapping_ = CreateFileMappingA(file_, nullptr, PAGE_READWRITE, 0, DK2M_FILE_SIZE, nullptr);
        if (!mapping_) return false;
        view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, DK2M_FILE_SIZE);
        if (!view_) return false;

        header_ = static_cast<DK2MFileHeader *>(view_);
        if (header_->magic != DK2M_MAGIC || header_->version != DK2M_VERSION ||
            header_->header_size != sizeof(DK2MFileHeader) || header_->file_size != DK2M_FILE_SIZE) {
            std::memset(view_, 0, DK2M_FILE_SIZE);
            header_->magic = DK2M_MAGIC;
            header_->version = DK2M_VERSION;
            header_->header_size = sizeof(DK2MFileHeader);
            header_->file_size = DK2M_FILE_SIZE;
            header_->latest_slot = DK2M_NO_SLOT;
        }
        previousSlot_ = InterlockedCompareExchange(asLong(&header_->latest_slot), 0, 0);
        if (previousSlot_ >= DK2M_SLOT_COUNT) previousSlot_ = DK2M_NO_SLOT;
        return true;
    }

    bool captureTexture(IDirectDrawSurface4 *surface, TextureCache &texture) {
        if (!surface) return false;
        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        const HRESULT hr = surface->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_READONLY, nullptr);
        if (FAILED(hr)) return false;

        bool valid = desc.lpSurface && desc.dwWidth && desc.dwHeight &&
                     desc.dwWidth <= 8192 && desc.dwHeight <= 8192 &&
                     desc.ddpfPixelFormat.dwRGBBitCount == 32 && desc.lPitch >= 0;
        const uint64_t rowPitch = static_cast<uint64_t>(desc.dwWidth) * 4;
        const uint64_t dataSize = rowPitch * desc.dwHeight;
        if (dataSize > DK2M_SLOT_CAPACITY - sizeof(DK2MTextureUpdateCommand)) valid = false;
        if (valid) {
            texture.width = desc.dwWidth;
            texture.height = desc.dwHeight;
            texture.rowPitch = static_cast<uint32_t>(rowPitch);
            texture.pixels.resize(static_cast<size_t>(dataSize));
            const auto *source = static_cast<const uint8_t *>(desc.lpSurface);
            for (uint32_t y = 0; y < texture.height; ++y) {
                uint8_t *destination = texture.pixels.data() + static_cast<size_t>(y) * texture.rowPitch;
                std::memcpy(destination, source + static_cast<size_t>(y) * desc.lPitch, texture.rowPitch);
                if (desc.ddpfPixelFormat.dwRGBAlphaBitMask == 0) {
                    for (uint32_t x = 0; x < texture.width; ++x) destination[x * 4 + 3] = 0xFF;
                }
            }
        }
        surface->Unlock(nullptr);
        return valid;
    }

    void emitTexture(DWORD stage, DWORD textureId) {
        if (textureId) {
            auto found = textures_.find(textureId);
            if (found != textures_.end()) {
                TextureCache &texture = found->second;
                const uint32_t consumer = InterlockedCompareExchange(asLong(&header_->consumer_frame), 0, 0);
                if (texture.pending && texture.lastSentFrame && consumer == texture.lastSentFrame) {
                    texture.pending = false;
                }
                if (texture.pending && !texture.sentInCurrentFrame) {
                    const uint32_t commandSize = static_cast<uint32_t>(
                        sizeof(DK2MTextureUpdateCommand) + texture.pixels.size());
                    if (used_ + commandSize <= DK2M_SLOT_CAPACITY) {
                        DK2MTextureUpdateCommand update = {};
                        update.header.type = DK2M_COMMAND_TEXTURE_UPDATE;
                        update.header.size = commandSize;
                        update.texture_id = textureId;
                        update.width = texture.width;
                        update.height = texture.height;
                        update.row_pitch = texture.rowPitch;
                        update.data_size = static_cast<uint32_t>(texture.pixels.size());
                        append(&update, sizeof(update));
                        append(texture.pixels.data(), update.data_size);
                        ++commandCount_;
                        texture.sentInCurrentFrame = true;
                    }
                }
            }
        }

        if (used_ + sizeof(DK2MSetTextureCommand) > DK2M_SLOT_CAPACITY) return;
        DK2MSetTextureCommand binding = {};
        binding.header.type = DK2M_COMMAND_SET_TEXTURE;
        binding.header.size = sizeof(binding);
        binding.stage = stage;
        binding.texture_id = textureId;
        append(&binding, sizeof(binding));
        ++commandCount_;
    }

    void emitRenderState(DWORD state, DWORD value) {
        if (used_ + sizeof(DK2MRenderStateCommand) > DK2M_SLOT_CAPACITY) return;
        DK2MRenderStateCommand command = {};
        command.header.type = DK2M_COMMAND_RENDER_STATE;
        command.header.size = sizeof(command);
        command.state = state;
        command.value = value;
        append(&command, sizeof(command));
        ++commandCount_;
    }

    void append(const void *data, uint32_t size) {
        std::memcpy(static_cast<uint8_t *>(view_) + DK2M_SLOT_OFFSET(slotIndex_) + used_, data, size);
        used_ += size;
    }

    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    void *view_ = nullptr;
    DK2MFileHeader *header_ = nullptr;
    DK2MFrameSlot *slot_ = nullptr;
    uint32_t previousSlot_ = DK2M_NO_SLOT;
    uint32_t slotIndex_ = 0;
    uint32_t sequence_ = 0;
    uint32_t frame_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t used_ = 0;
    uint32_t commandCount_ = 0;
    uint32_t boundTextures_[3] = {};
    uint32_t lastConsumerSession_ = 0;
    uint32_t lastConsumerFrame_ = 0;
    std::unordered_map<uint32_t, TextureCache> textures_;
    std::unordered_map<uintptr_t, uint32_t> surfaceTextures_;
    std::unordered_map<uint32_t, uint32_t> renderStates_;
    bool active_ = false;
};

Producer producer;

} // namespace

bool isEnabled() {
    char path[2];
    return GetEnvironmentVariableA("DK2_METAL_BRIDGE_FILE", path, sizeof(path)) != 0;
}

void beginFrame(DWORD width, DWORD height) {
    producer.begin(width, height);
}

void drawIndexed(DWORD fvf, const void *vertices, DWORD vertexCount,
                 const WORD *indices, DWORD indexCount, DWORD flags) {
    producer.draw(fvf, vertices, vertexCount, indices, indexCount, flags);
}

void setTexture(DWORD stage, DWORD textureId, IDirectDrawSurface4 *surface) {
    producer.texture(stage, textureId, surface);
}

void textureDirty(IDirectDrawSurface4 *surface) {
    producer.textureDirty(surface);
}

void setRenderState(DWORD state, DWORD value) {
    producer.renderState(state, value);
}

bool getRenderState(DWORD state, DWORD *value) {
    return producer.renderState(state, value);
}

void endFrame() {
    producer.finish();
}

} // namespace gog::metal_bridge
