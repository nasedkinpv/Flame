#include <metal_bridge/MetalBridgeProducer.h>
#include <metal_bridge/DK2BridgeProtocol.h>
#include <metal_bridge/OverlayUnmatte.h>
#include <gog_globals.h>
#include <patches/replace_mouse_dinput_to_user32.h>
#include <d3d.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gog::metal_bridge {
namespace {

using PhaseClock = std::chrono::steady_clock;

uint32_t phaseMicroseconds(PhaseClock::time_point started) {
    const auto value = std::chrono::duration_cast<std::chrono::microseconds>(
        PhaseClock::now() - started).count();
    return value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(std::max<int64_t>(0, value));
}

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

constexpr uint32_t kOverlayTileSize = 32;

struct OverlayTileState {
    uint32_t version = 1;
    uint32_t sentVersion = 0;
    uint32_t acknowledgedVersion = 0;
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
        overlayWhite_ = !overlayWhite_;
        used_ = 0;
        commandCount_ = 0;
        drawCopyMicroseconds_ = 0;
        textureMicroseconds_ = 0;
        overlayMicroseconds_ = 0;
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
        for (const auto &entry : textureStageStates_) {
            emitTextureStageState(static_cast<DWORD>(entry.first >> 32),
                                  static_cast<DWORD>(entry.first), entry.second);
        }
    }

    void draw(DWORD fvf, const void *vertices, DWORD vertexCount,
              const WORD *indices, DWORD indexCount, DWORD flags) {
        if (!active_ || !vertices || !indices) return;
        uint32_t vertexSize;
        if (fvf == DK2M_FVF_VERTEX1C) vertexSize = sizeof(DK2MVertex1C);
        else if (fvf == DK2M_FVF_VERTEX2C) vertexSize = sizeof(DK2MVertex2C);
        else return;
        const uint64_t vertexBytes = static_cast<uint64_t>(vertexCount) * vertexSize;
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
                if (captureTexture(surface, updated)) {
                    updateTexture(found->second, std::move(updated));
                }
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
            updateTexture(texture->second, std::move(updated));
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

    void textureStageState(DWORD stage, DWORD state, DWORD value) {
        const uint64_t key = (static_cast<uint64_t>(stage) << 32) | state;
        textureStageStates_[key] = value;
        if (active_) emitTextureStageState(stage, state, value);
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
        const auto overlayStarted = PhaseClock::now();
        emitOverlay();
        addOverlayTiming(phaseMicroseconds(overlayStarted));
        ++frame_;
        if (frame_ == 0) ++frame_;
        slot_->frame_number = frame_;
        slot_->byte_count = used_;
        slot_->command_count = commandCount_;
        slot_->width = width_;
        slot_->height = height_;
        const uint32_t sceneMicroseconds = elapsedMicroseconds(sceneStarted_, timerTicks());
        slot_->game_timings[0] = packMicroseconds(sceneMicroseconds) |
                                 (packMicroseconds(gameTickMicroseconds_) << 16);
        slot_->game_timings[1] = packMicroseconds(prepareMicroseconds_) |
                                 (packMicroseconds(drawMicroseconds_) << 16);
        slot_->producer_timings[0] = packMicroseconds(drawCopyMicroseconds_) |
                                     (packMicroseconds(textureMicroseconds_) << 16);
        slot_->producer_timings[1] = packMicroseconds(overlayMicroseconds_);
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

    void overlay(IDirectDrawSurface4 *surface) {
        if (!captureTexture(surface, overlayCapture_)) return;
        if (!previousOverlay_.pixels.empty() && previousOverlayWhite_ != overlayWhite_ &&
            overlayCapture_.width == previousOverlay_.width &&
            overlayCapture_.height == previousOverlay_.height &&
            overlayCapture_.rowPitch == previousOverlay_.rowPitch) {
            const TextureCache &black = overlayWhite_ ? previousOverlay_ : overlayCapture_;
            const TextureCache &white = overlayWhite_ ? overlayCapture_ : previousOverlay_;
            updateOverlay(black, white);
        }
        std::swap(previousOverlay_, overlayCapture_);
        previousOverlayWhite_ = overlayWhite_;
    }

    DWORD overlayClearColor() const { return overlayWhite_ ? 0x00FFFFFF : 0x00000000; }

    void pollInput() {
        if (ensureMapped()) processInput();
    }

    void addDrawCopyTiming(uint32_t microseconds) {
        addTiming(drawCopyMicroseconds_, microseconds);
    }

    void addTextureTiming(uint32_t microseconds) {
        addTiming(textureMicroseconds_, microseconds);
    }

    void addOverlayTiming(uint32_t microseconds) {
        addTiming(overlayMicroseconds_, microseconds);
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

    static bool sameTexture(const TextureCache &left, const TextureCache &right) {
        return left.width == right.width && left.height == right.height &&
               left.rowPitch == right.rowPitch &&
               left.pixels.size() == right.pixels.size() &&
               (left.pixels.empty() ||
                std::memcmp(left.pixels.data(), right.pixels.data(),
                            left.pixels.size()) == 0);
    }

    static void updateTexture(TextureCache &current, TextureCache &&updated) {
        if (sameTexture(current, updated)) {
            // DK2 frequently locks and rewrites unchanged UI surfaces. Preserve
            // their acknowledgement state so they do not consume the bridge's
            // texture-upload budget every frame.
            current.dirty = false;
            return;
        }
        current = std::move(updated);
    }

    void updateOverlay(const TextureCache &black, const TextureCache &white) {
        if (overlay_.width != black.width || overlay_.height != black.height ||
            overlay_.rowPitch != black.rowPitch) {
            overlay_.width = black.width;
            overlay_.height = black.height;
            overlay_.rowPitch = black.rowPitch;
            overlay_.pixels.assign(static_cast<size_t>(black.rowPitch) * black.height, 0);
            overlayTileColumns_ = (black.width + kOverlayTileSize - 1) / kOverlayTileSize;
            overlayTileRows_ = (black.height + kOverlayTileSize - 1) / kOverlayTileSize;
            overlayTiles_.assign(static_cast<size_t>(overlayTileColumns_) * overlayTileRows_, {});
            overlayChanged_.resize(overlayTiles_.size());
            overlayLine_.resize(black.rowPitch);
            overlayForceFull_ = true;
        }

        std::fill(overlayChanged_.begin(), overlayChanged_.end(), 0);
        for (uint32_t y = 0; y < black.height; ++y) {
            const uint8_t *blackRow = black.pixels.data() + static_cast<size_t>(y) * black.rowPitch;
            const uint8_t *whiteRow = white.pixels.data() + static_cast<size_t>(y) * white.rowPitch;
            uint8_t *overlayRow = overlay_.pixels.data() + static_cast<size_t>(y) * overlay_.rowPitch;
            unmatteOverlaySpan(blackRow, whiteRow, overlayLine_.data(), black.width);
            const uint32_t tileY = y / kOverlayTileSize;
            for (uint32_t tileX = 0; tileX < overlayTileColumns_; ++tileX) {
                const uint32_t x = tileX * kOverlayTileSize;
                const uint32_t width = std::min(kOverlayTileSize, black.width - x);
                const size_t bytes = static_cast<size_t>(width) * 4;
                if (std::memcmp(overlayRow + static_cast<size_t>(x) * 4,
                                overlayLine_.data() + static_cast<size_t>(x) * 4,
                                bytes) == 0) continue;
                std::memcpy(overlayRow + static_cast<size_t>(x) * 4,
                            overlayLine_.data() + static_cast<size_t>(x) * 4, bytes);
                overlayChanged_[static_cast<size_t>(tileY) * overlayTileColumns_ + tileX] = 1;
            }
        }
        for (size_t tile = 0; tile < overlayTiles_.size(); ++tile) {
            if (!overlayChanged_[tile]) continue;
            if (++overlayTiles_[tile].version == 0) overlayTiles_[tile].version = 1;
        }
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

    static uint32_t packMicroseconds(uint32_t value) {
        const uint32_t quantized = (value + DK2M_TIMING_QUANTUM_US / 2) /
                                   DK2M_TIMING_QUANTUM_US;
        return quantized > UINT16_MAX ? UINT16_MAX : quantized;
    }

    static void addTiming(uint32_t &total, uint32_t value) {
        total = value > UINT32_MAX - total ? UINT32_MAX : total + value;
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

    void emitTextureStageState(DWORD stage, DWORD state, DWORD value) {
        if (used_ + sizeof(DK2MTextureStageStateCommand) > DK2M_SLOT_CAPACITY) return;
        DK2MTextureStageStateCommand command = {};
        command.header.type = DK2M_COMMAND_TEXTURE_STAGE_STATE;
        command.header.size = sizeof(command);
        command.stage = stage;
        command.state = state;
        command.value = value;
        append(&command, sizeof(command));
        ++commandCount_;
    }

    bool overlayTilePending(uint32_t tileX, uint32_t tileY) const {
        const OverlayTileState &tile =
            overlayTiles_[static_cast<size_t>(tileY) * overlayTileColumns_ + tileX];
        return tile.version != tile.acknowledgedVersion;
    }

    uint64_t overlayRectBytes() const {
        uint64_t bytes = 0;
        for (uint32_t tileY = 0; tileY < overlayTileRows_; ++tileY) {
            const uint32_t height = std::min(kOverlayTileSize,
                                             overlay_.height - tileY * kOverlayTileSize);
            for (uint32_t tileX = 0; tileX < overlayTileColumns_;) {
                if (!overlayTilePending(tileX, tileY)) {
                    ++tileX;
                    continue;
                }
                const uint32_t first = tileX++;
                while (tileX < overlayTileColumns_ && overlayTilePending(tileX, tileY)) ++tileX;
                const uint32_t x = first * kOverlayTileSize;
                const uint32_t width = std::min(overlay_.width, tileX * kOverlayTileSize) - x;
                bytes += sizeof(DK2MTextureUpdateRectCommand) +
                         static_cast<uint64_t>(width) * height * 4;
            }
        }
        return bytes;
    }

    void markOverlaySent(uint32_t firstTileX, uint32_t lastTileX, uint32_t tileY) {
        for (uint32_t tileX = firstTileX; tileX < lastTileX; ++tileX) {
            OverlayTileState &tile =
                overlayTiles_[static_cast<size_t>(tileY) * overlayTileColumns_ + tileX];
            tile.sentVersion = tile.version;
        }
    }

    void emitOverlayRects() {
        for (uint32_t tileY = 0; tileY < overlayTileRows_; ++tileY) {
            const uint32_t y = tileY * kOverlayTileSize;
            const uint32_t height = std::min(kOverlayTileSize, overlay_.height - y);
            for (uint32_t tileX = 0; tileX < overlayTileColumns_;) {
                if (!overlayTilePending(tileX, tileY)) {
                    ++tileX;
                    continue;
                }
                const uint32_t first = tileX++;
                while (tileX < overlayTileColumns_ && overlayTilePending(tileX, tileY)) ++tileX;
                const uint32_t x = first * kOverlayTileSize;
                const uint32_t width = std::min(overlay_.width, tileX * kOverlayTileSize) - x;
                DK2MTextureUpdateRectCommand update = {};
                update.header.type = DK2M_COMMAND_TEXTURE_UPDATE_RECT;
                update.row_pitch = width * 4;
                update.data_size = update.row_pitch * height;
                update.header.size = sizeof(update) + update.data_size;
                update.texture_id = DK2M_OVERLAY_TEXTURE_ID;
                update.x = x;
                update.y = y;
                update.width = width;
                update.height = height;
                append(&update, sizeof(update));
                for (uint32_t row = 0; row < height; ++row) {
                    append(overlay_.pixels.data() +
                               static_cast<size_t>(y + row) * overlay_.rowPitch +
                               static_cast<size_t>(x) * 4,
                           update.row_pitch);
                }
                ++commandCount_;
                markOverlaySent(first, tileX, tileY);
            }
        }
    }

    void emitOverlayFull() {
        DK2MTextureUpdateCommand update = {};
        update.header.type = DK2M_COMMAND_TEXTURE_UPDATE;
        update.header.size = sizeof(update) + static_cast<uint32_t>(overlay_.pixels.size());
        update.texture_id = DK2M_OVERLAY_TEXTURE_ID;
        update.width = overlay_.width;
        update.height = overlay_.height;
        update.row_pitch = overlay_.rowPitch;
        update.data_size = static_cast<uint32_t>(overlay_.pixels.size());
        append(&update, sizeof(update));
        append(overlay_.pixels.data(), update.data_size);
        ++commandCount_;
        for (OverlayTileState &tile : overlayTiles_) tile.sentVersion = tile.version;
        overlayForceFull_ = false;
    }

    void emitOverlay() {
        if (overlay_.pixels.empty()) return;
        constexpr WORD indices[] = {0, 1, 2, 0, 2, 3};
        const DK2MVertex1C vertices[] = {
            {0.0f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f},
            {static_cast<float>(width_), 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
            {static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f,
             0xFFFFFFFFu, 1.0f, 1.0f},
            {0.0f, static_cast<float>(height_), 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
        };
        const uint32_t drawBytes = sizeof(DK2MDrawIndexedCommand) + sizeof(vertices) + sizeof(indices);
        constexpr uint32_t stateBytes =
            sizeof(DK2MSetTextureCommand) + 6 * sizeof(DK2MRenderStateCommand) +
            6 * sizeof(DK2MTextureStageStateCommand);
        if (used_ + drawBytes + stateBytes > DK2M_SLOT_CAPACITY) return;

        const uint32_t consumerSession =
            InterlockedCompareExchange(asLong(&header_->consumer_session), 0, 0);
        if (consumerSession != overlayConsumerSession_) {
            overlayConsumerSession_ = consumerSession;
            overlayLastSentFrame_ = 0;
            overlayForceFull_ = true;
            for (OverlayTileState &tile : overlayTiles_) tile.acknowledgedVersion = 0;
        }
        const uint32_t consumerFrame =
            InterlockedCompareExchange(asLong(&header_->consumer_frame), 0, 0);
        if (overlayLastSentFrame_) {
            if (consumerFrame == overlayLastSentFrame_) {
                for (OverlayTileState &tile : overlayTiles_) {
                    if (tile.sentVersion) tile.acknowledgedVersion = tile.sentVersion;
                    tile.sentVersion = 0;
                }
                overlayLastSentFrame_ = 0;
            } else if (static_cast<int32_t>(consumerFrame - overlayLastSentFrame_) > 0) {
                // The consumer accepted a newer producer frame without seeing
                // the one that carried this update. Keep the versions pending
                // and allow one retry instead of chasing the consumer forever.
                for (OverlayTileState &tile : overlayTiles_) tile.sentVersion = 0;
                overlayLastSentFrame_ = 0;
            }
        }

        const uint64_t fullBytes = sizeof(DK2MTextureUpdateCommand) + overlay_.pixels.size();
        const uint64_t rectBytes = overlayRectBytes();
        const uint64_t updateBytes = overlayForceFull_ || rectBytes >= fullBytes
                                         ? fullBytes : rectBytes;
        if (!overlayLastSentFrame_ && updateBytes &&
            used_ + drawBytes + stateBytes + updateBytes <= DK2M_SLOT_CAPACITY) {
            if (overlayForceFull_ || rectBytes >= fullBytes) emitOverlayFull();
            else emitOverlayRects();
            overlayLastSentFrame_ = frame_ + 1;
            if (overlayLastSentFrame_ == 0) overlayLastSentFrame_ = 1;
        }
        emitTexture(0, DK2M_OVERLAY_TEXTURE_ID);
        emitRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
        emitRenderState(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
        emitRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
        emitRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
        emitRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
        emitRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
        emitTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        emitTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        emitTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        emitTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        emitTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        emitTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        draw(DK2M_FVF_VERTEX1C, vertices, 4, indices, 6, 0);
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
    uint32_t drawCopyMicroseconds_ = 0;
    uint32_t textureMicroseconds_ = 0;
    uint32_t overlayMicroseconds_ = 0;
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
    std::unordered_map<uint64_t, uint32_t> textureStageStates_;
    TextureCache overlay_;
    TextureCache previousOverlay_;
    TextureCache overlayCapture_;
    std::vector<OverlayTileState> overlayTiles_;
    std::vector<uint8_t> overlayChanged_;
    std::vector<uint8_t> overlayLine_;
    uint32_t overlayTileColumns_ = 0;
    uint32_t overlayTileRows_ = 0;
    uint32_t overlayConsumerSession_ = 0;
    uint32_t overlayLastSentFrame_ = 0;
    bool overlayForceFull_ = true;
    bool overlayWhite_ = false;
    bool previousOverlayWhite_ = false;
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

DWORD overlayClearColor() {
    return producer.overlayClearColor();
}

void drawIndexed(DWORD fvf, const void *vertices, DWORD vertexCount,
                 const WORD *indices, DWORD indexCount, DWORD flags) {
    const auto started = PhaseClock::now();
    producer.draw(fvf, vertices, vertexCount, indices, indexCount, flags);
    producer.addDrawCopyTiming(phaseMicroseconds(started));
}

void captureOverlay(IDirectDrawSurface4 *surface) {
    const auto started = PhaseClock::now();
    producer.overlay(surface);
    producer.addOverlayTiming(phaseMicroseconds(started));
}

void setTexture(DWORD stage, DWORD textureId, IDirectDrawSurface4 *surface) {
    const auto started = PhaseClock::now();
    producer.texture(stage, textureId, surface);
    producer.addTextureTiming(phaseMicroseconds(started));
}

void textureDirty(IDirectDrawSurface4 *surface, const DDSURFACEDESC2 *lockedDesc) {
    const auto started = PhaseClock::now();
    producer.textureDirty(surface, lockedDesc);
    producer.addTextureTiming(phaseMicroseconds(started));
}

void setRenderState(DWORD state, DWORD value) {
    producer.renderState(state, value);
}

bool getRenderState(DWORD state, DWORD *value) {
    return producer.renderState(state, value);
}

void setTextureStageState(DWORD stage, DWORD state, DWORD value) {
    producer.textureStageState(stage, state, value);
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
