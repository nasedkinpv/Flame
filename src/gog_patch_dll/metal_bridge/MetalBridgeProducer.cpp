#include <metal_bridge/MetalBridgeProducer.h>
#include <metal_bridge/DK2BridgeProtocol.h>
#include <gog_globals.h>
#include <patches/replace_mouse_dinput_to_user32.h>

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
        sceneStarted_ = timerTicks();

        const uint32_t consumer = InterlockedCompareExchange(asLong(&header_->consumer_frame), 0, 0);
        const uint32_t consumerSession =
            InterlockedCompareExchange(asLong(&header_->consumer_session), 0, 0);
        if (consumerSession != lastConsumerSession_ || consumer < lastConsumerFrame_) {
            for (auto &entry : textures_) entry.second.pending = true;
        }
        lastConsumerSession_ = consumerSession;
        lastConsumerFrame_ = consumer;
        pollInput();

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

    void textureDirty(IDirectDrawSurface4 *surface, const DDSURFACEDESC2 *lockedDesc) {
        if (!surface) return;
        const auto surfaceEntry = surfaceTextures_.find(reinterpret_cast<uintptr_t>(surface));
        if (surfaceEntry == surfaceTextures_.end()) return;
        const auto texture = textures_.find(surfaceEntry->second);
        if (texture == textures_.end()) return;

        TextureCache updated;
        if (lockedDesc && copyTexture(*lockedDesc, updated)) {
            texture->second = std::move(updated);
        } else {
            texture->second.dirty = true;
        }
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

    void gameTickTiming(uint32_t tickMicroseconds) {
        gameTickMicroseconds_ = tickMicroseconds;
    }

    void gameRenderTimings(uint32_t prepareMicroseconds, uint32_t drawMicroseconds) {
        prepareMicroseconds_ = prepareMicroseconds;
        drawMicroseconds_ = drawMicroseconds;
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
        const uint32_t sceneMicroseconds = elapsedMicroseconds(sceneStarted_, timerTicks());
        slot_->reserved[0] = clampMicroseconds(sceneMicroseconds) |
                             (clampMicroseconds(gameTickMicroseconds_) << 16);
        slot_->reserved[1] = clampMicroseconds(prepareMicroseconds_) |
                             (clampMicroseconds(drawMicroseconds_) << 16);
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

    void pollInput() {
        if (ensureMapped()) processInput();
    }

private:
    static volatile LONG *asLong(volatile uint32_t *value) {
        return reinterpret_cast<volatile LONG *>(value);
    }

    bool readInput(DK2MInputState &input) const {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const uint32_t before = InterlockedCompareExchange(
                asLong(&header_->input.sequence), 0, 0);
            if ((before & 1u) != 0) continue;
            std::memcpy(&input, &header_->input, sizeof(input));
            MemoryBarrier();
            const uint32_t after = InterlockedCompareExchange(
                asLong(&header_->input.sequence), 0, 0);
            if (before == after && (after & 1u) == 0) return true;
        }
        return false;
    }

    void applyButton(uint32_t button, int32_t value) {
        if (button >= 4) return;
        patch::replace_mouse_dinput_to_user32::inject_metal_button(button, value);
        if (value == 0) appliedButtons_ &= ~(1u << button);
        else appliedButtons_ |= 1u << button;
    }

    void applyKey(uint32_t key, bool pressed) {
        if (key == 0 || key >= 256) return;
        patch::replace_mouse_dinput_to_user32::inject_metal_key(key, pressed);
        const uint8_t mask = static_cast<uint8_t>(1u << (key & 7));
        if (pressed) appliedKeys_[key >> 3] |= mask;
        else appliedKeys_[key >> 3] &= static_cast<uint8_t>(~mask);
    }

    void releaseAppliedInput() {
        for (uint32_t button = 0; button < 4; ++button) {
            if (appliedButtons_ & (1u << button)) applyButton(button, 0);
        }
        for (uint32_t key = 1; key < 256; ++key) {
            if (appliedKeys_[key >> 3] & (1u << (key & 7))) applyKey(key, false);
        }
    }

    void injectWheel(int32_t delta) {
        while (delta != 0 && gog::g_hWnd) {
            const int32_t chunk = delta > 32760 ? 32760 : (delta < -32760 ? -32760 : delta);
            const WPARAM keysAndDelta = MAKEWPARAM(0, static_cast<WORD>(static_cast<SHORT>(chunk)));
            SendMessageA(gog::g_hWnd, WM_MOUSEWHEEL, keysAndDelta, 0);
            delta -= chunk;
        }
    }

    void processInput() {
        DK2MInputState input = {};
        if (!readInput(input)) return;
        if (input.host_pid == 0) {
            if (inputHostPid_ != 0) {
                releaseAppliedInput();
                inputHostPid_ = 0;
            }
            return;
        }
        const bool newHost = input.host_pid != inputHostPid_;
        const DWORD now = GetTickCount();
        if (newHost) {
            releaseAppliedInput();
            inputHostPid_ = input.host_pid;
            inputEventWrite_ = input.event_write;
            lastRelativeX_ = input.relative_x;
            lastRelativeY_ = input.relative_y;
            lastWheelX_ = input.wheel_x;
            lastWheelY_ = input.wheel_y;
            lastInputHeartbeat_ = input.heartbeat;
            lastInputHeartbeatTick_ = now;
        } else if (input.heartbeat != lastInputHeartbeat_) {
            lastInputHeartbeat_ = input.heartbeat;
            lastInputHeartbeatTick_ = now;
        } else if (now - lastInputHeartbeatTick_ > 2000) {
            releaseAppliedInput();
            inputHostPid_ = 0;
            return;
        }

        const bool active = (input.flags & DK2M_INPUT_ACTIVE) != 0;
        if (!newHost && active) {
            const uint32_t eventCount = input.event_write - inputEventWrite_;
            if (eventCount <= 4) {
                for (uint32_t serial = inputEventWrite_ + 1; serial != input.event_write + 1; ++serial) {
                    const DK2MInputEvent &event = input.events[(serial - 1) % 4];
                    if (event.type == DK2M_INPUT_EVENT_BUTTON) applyButton(event.code, event.value);
                    else if (event.type == DK2M_INPUT_EVENT_KEY) applyKey(event.code, event.value != 0);
                }
            }
        }
        inputEventWrite_ = input.event_write;

        const uint32_t desiredButtons = active ? input.buttons : 0;
        for (uint32_t button = 0; button < 4; ++button) {
            const bool desired = (desiredButtons & (1u << button)) != 0;
            const bool applied = (appliedButtons_ & (1u << button)) != 0;
            if (desired != applied) applyButton(button, desired ? 1 : 0);
        }
        for (uint32_t key = 1; key < 256; ++key) {
            const bool desired = active && (input.keys[key >> 3] & (1u << (key & 7))) != 0;
            const bool applied = (appliedKeys_[key >> 3] & (1u << (key & 7))) != 0;
            if (desired != applied) applyKey(key, desired);
        }

        const int32_t deltaX = static_cast<int32_t>(input.relative_x - lastRelativeX_);
        const int32_t deltaY = static_cast<int32_t>(input.relative_y - lastRelativeY_);
        const int32_t wheelX = static_cast<int32_t>(input.wheel_x - lastWheelX_);
        const int32_t wheelY = static_cast<int32_t>(input.wheel_y - lastWheelY_);
        lastRelativeX_ = input.relative_x;
        lastRelativeY_ = input.relative_y;
        lastWheelX_ = input.wheel_x;
        lastWheelY_ = input.wheel_y;
        if (active && (input.flags & DK2M_INPUT_CURSOR_VALID) != 0) {
            patch::replace_mouse_dinput_to_user32::inject_metal_pointer(
                input.cursor_x, input.cursor_y, deltaX, deltaY);
        }
        if (active) {
            (void)wheelX;
            injectWheel(wheelY);
        }
        InterlockedExchange(asLong(&header_->input_ack_sequence), static_cast<LONG>(input.sequence));
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
        QueryPerformanceFrequency(&timerFrequency_);
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

    static bool copyTexture(const DDSURFACEDESC2 &desc, TextureCache &texture) {
        bool valid = desc.lpSurface && desc.dwWidth && desc.dwHeight &&
                     desc.dwWidth <= 8192 && desc.dwHeight <= 8192 &&
                     desc.ddpfPixelFormat.dwRGBBitCount == 32 && desc.lPitch >= 0;
        const uint64_t rowPitch = static_cast<uint64_t>(desc.dwWidth) * 4;
        const uint64_t dataSize = rowPitch * desc.dwHeight;
        if (dataSize > DK2M_SLOT_CAPACITY - sizeof(DK2MTextureUpdateCommand)) valid = false;
        if (!valid) return false;

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
        return true;
    }

    bool captureTexture(IDirectDrawSurface4 *surface, TextureCache &texture) {
        if (!surface) return false;
        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        const HRESULT hr = surface->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_READONLY, nullptr);
        if (FAILED(hr)) return false;
        const bool valid = copyTexture(desc, texture);
        surface->Unlock(nullptr);
        return valid;
    }

    uint64_t timerTicks() const {
        LARGE_INTEGER value = {};
        QueryPerformanceCounter(&value);
        return static_cast<uint64_t>(value.QuadPart);
    }

    uint32_t elapsedMicroseconds(uint64_t started, uint64_t ended) const {
        if (timerFrequency_.QuadPart <= 0 || ended < started) return 0;
        const uint64_t value = (ended - started) * 1000000u /
                               static_cast<uint64_t>(timerFrequency_.QuadPart);
        return value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
    }

    static uint32_t clampMicroseconds(uint32_t value) {
        return value > UINT16_MAX ? UINT16_MAX : value;
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
    LARGE_INTEGER timerFrequency_ = {};
    uint64_t sceneStarted_ = 0;
    uint32_t gameTickMicroseconds_ = 0;
    uint32_t prepareMicroseconds_ = 0;
    uint32_t drawMicroseconds_ = 0;
    uint32_t boundTextures_[3] = {};
    uint32_t lastConsumerSession_ = 0;
    uint32_t lastConsumerFrame_ = 0;
    uint32_t inputHostPid_ = 0;
    uint32_t inputEventWrite_ = 0;
    uint32_t lastRelativeX_ = 0;
    uint32_t lastRelativeY_ = 0;
    uint32_t lastWheelX_ = 0;
    uint32_t lastWheelY_ = 0;
    uint32_t appliedButtons_ = 0;
    uint32_t lastInputHeartbeat_ = 0;
    DWORD lastInputHeartbeatTick_ = 0;
    uint8_t appliedKeys_[32] = {};
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

void pollInput() {
    producer.pollInput();
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

void textureDirty(IDirectDrawSurface4 *surface, const DDSURFACEDESC2 *lockedDesc) {
    producer.textureDirty(surface, lockedDesc);
}

void setRenderState(DWORD state, DWORD value) {
    producer.renderState(state, value);
}

bool getRenderState(DWORD state, DWORD *value) {
    return producer.renderState(state, value);
}

void setGameTickTiming(uint32_t tickMicroseconds) {
    producer.gameTickTiming(tickMicroseconds);
}

void setGameRenderTimings(uint32_t prepareMicroseconds, uint32_t drawMicroseconds) {
    producer.gameRenderTimings(prepareMicroseconds, drawMicroseconds);
}

void endFrame() {
    producer.finish();
}

} // namespace gog::metal_bridge
