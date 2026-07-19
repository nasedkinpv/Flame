#include <metal_bridge/MetalBridgeProducer.h>
#include <metal_bridge/DK2BridgeProtocol.h>
#include <metal_bridge/OverlayUnmatte.h>
#include <gog_globals.h>
#include <gog_debug.h>
#include <patches/replace_mouse_dinput_to_user32.h>
#include <d3d.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
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

struct CursorSnapshot {
    RECT rect = {};
    uint32_t width = 0;
    uint32_t height = 0;
    bool visible = false;
    bool matteWhite = false;
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> background;
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
        {
            std::lock_guard<std::mutex> lock(overlayMutex_);
            frameThreadId_ = GetCurrentThreadId();
            overlayWhite_ = !overlayWhite_;
        }
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

    void overlayCleared() {
        std::lock_guard<std::mutex> lock(overlayMutex_);
        drawnValid_ = false;
        clearedThisFrame_ = true;
    }

    void overlayDrawn(const RECT *rect) {
        std::lock_guard<std::mutex> lock(overlayMutex_);
        if (!rect) {
            drawnFull_ = true;
            return;
        }
        if (rect->right <= rect->left || rect->bottom <= rect->top) return;
        if (!drawnValid_) {
            drawnRect_ = *rect;
            drawnValid_ = true;
        } else {
            drawnRect_.left = std::min(drawnRect_.left, rect->left);
            drawnRect_.top = std::min(drawnRect_.top, rect->top);
            drawnRect_.right = std::max(drawnRect_.right, rect->right);
            drawnRect_.bottom = std::max(drawnRect_.bottom, rect->bottom);
        }
    }

    void overlayBltFast(IDirectDrawSurface4 *destination, DWORD x, DWORD y,
                        IDirectDrawSurface4 *source, const RECT *sourceRect,
                        DWORD flags) {
        {
            std::lock_guard<std::mutex> lock(overlayMutex_);
            if (!frameThreadId_ || GetCurrentThreadId() == frameThreadId_) return;
            if ((flags & DDBLTFAST_SRCCOLORKEY) == 0) {
                // The cursor worker first restores the background, then draws
                // the new colour-keyed icon.  Publishing hidden here also
                // handles the game's explicit hide path.
                cursor_.visible = false;
                return;
            }
        }

        CursorSnapshot captured;
        if (!captureCursor(destination, x, y, source, sourceRect, captured)) {
            std::lock_guard<std::mutex> lock(overlayMutex_);
            if (!cursorCaptureFailureLogged_) {
                gog_debug("Metal cursor: colour-keyed worker Blt detected but capture is unsupported");
                cursorCaptureFailureLogged_ = true;
            }
            return;
        }
        std::lock_guard<std::mutex> lock(overlayMutex_);
        captured.matteWhite = overlayWhite_;
        if (!cursorCaptureLogged_) {
            gog_debugf("Metal cursor: separated %ux%u sprite from temporal overlay",
                       captured.width, captured.height);
            cursorCaptureLogged_ = true;
        }
        cursor_ = std::move(captured);
    }

    static RECT unionRect(const RECT &a, const RECT &b) {
        return {std::min(a.left, b.left), std::min(a.top, b.top),
                std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
    }

    void overlay(IDirectDrawSurface4 *surface) {
        int parity;
        bool tracked;
        RECT drawn;
        bool haveDrawn;
        CursorSnapshot cursor;
        {
            std::lock_guard<std::mutex> lock(overlayMutex_);
            parity = overlayWhite_ ? 1 : 0;
            tracked = clearedThisFrame_ && !drawnFull_;
            clearedThisFrame_ = false;
            drawnFull_ = false;
            drawn = drawnValid_ ? drawnRect_ : RECT{0, 0, 0, 0};
            haveDrawn = drawnValid_;
            drawnValid_ = false;
            cursor = cursor_;
        }
        TextureCache &cache = parityCache_[parity];

        RECT refreshed;
        if (tracked && !cache.pixels.empty()) {
            // partial path: everything outside the drawn region is the clear
            // colour; only refresh the previous and current drawn areas
            DDSURFACEDESC2 desc = {};
            desc.dwSize = sizeof(desc);
            if (FAILED(surface->Lock(nullptr, &desc, DDLOCK_READONLY | DDLOCK_WAIT, nullptr)))
                return;
            const bool dimsOk = desc.dwWidth == cache.width && desc.dwHeight == cache.height &&
                                desc.ddpfPixelFormat.dwRGBBitCount == 32;
            if (!dimsOk) {
                surface->Unlock(nullptr);
                if (!captureTexture(surface, cache)) return;
                parityRefresh_[parity] = {0, 0, (LONG)cache.width, (LONG)cache.height};
            } else {
                RECT refresh = parityDrawn_[parity].right > 0
                        ? (haveDrawn ? unionRect(parityDrawn_[parity], drawn) : parityDrawn_[parity])
                        : (haveDrawn ? drawn : RECT{0, 0, 0, 0});
                refresh.left = std::clamp<LONG>(refresh.left, 0, (LONG)cache.width);
                refresh.right = std::clamp<LONG>(refresh.right, 0, (LONG)cache.width);
                refresh.top = std::clamp<LONG>(refresh.top, 0, (LONG)cache.height);
                refresh.bottom = std::clamp<LONG>(refresh.bottom, 0, (LONG)cache.height);
                const DWORD clear = overlayClearColor() | 0xFF000000u;
                const bool opaqueAlpha = desc.ddpfPixelFormat.dwRGBAlphaBitMask == 0;
                const auto *src = static_cast<const uint8_t *>(desc.lpSurface);
                for (LONG y = refresh.top; y < refresh.bottom; ++y) {
                    auto *row = reinterpret_cast<uint32_t *>(
                            cache.pixels.data() + (size_t)y * cache.rowPitch);
                    for (LONG x = refresh.left; x < refresh.right; ++x) row[x] = clear;
                }
                if (haveDrawn) {
                    for (LONG y = drawn.top; y < drawn.bottom && y < (LONG)cache.height; ++y) {
                        if (y < 0) continue;
                        const LONG x0 = std::max<LONG>(drawn.left, 0);
                        const LONG x1 = std::min<LONG>(drawn.right, (LONG)cache.width);
                        if (x1 <= x0) break;
                        uint8_t *dst = cache.pixels.data() + (size_t)y * cache.rowPitch + x0 * 4;
                        std::memcpy(dst, src + (size_t)y * desc.lPitch + x0 * 4,
                                    (size_t)(x1 - x0) * 4);
                        if (opaqueAlpha) {
                            for (LONG x = 0; x < x1 - x0; ++x) dst[x * 4 + 3] = 0xFF;
                        }
                    }
                }
                surface->Unlock(nullptr);
                parityDrawn_[parity] = haveDrawn ? drawn : RECT{0, 0, 0, 0};
                parityRefresh_[parity] = refresh;
            }
        } else {
            if (!captureTexture(surface, cache)) return;
            parityDrawn_[parity] = {0, 0, (LONG)cache.width, (LONG)cache.height};
            parityRefresh_[parity] = {0, 0, (LONG)cache.width, (LONG)cache.height};
        }
        refreshed = parityRefresh_[parity];
        stripCursor(cache, cursor, parity != 0);

        TextureCache &other = parityCache_[1 - parity];
        if (!other.pixels.empty() && other.width == cache.width &&
            other.height == cache.height && other.rowPitch == cache.rowPitch) {
            const RECT process = unionRect(refreshed, parityRefresh_[1 - parity]);
            updateOverlay(parityCache_[0], parityCache_[1], process, cursor);
        }
    }

    DWORD overlayClearColor() {
        std::lock_guard<std::mutex> lock(overlayMutex_);
        return overlayWhite_ ? 0x00FFFFFF : 0x00000000;
    }

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

    static bool captureCursor(IDirectDrawSurface4 *destination, DWORD x, DWORD y,
                              IDirectDrawSurface4 *source, const RECT *sourceRect,
                              CursorSnapshot &cursor) {
        if (!destination || !source) return false;
        DDSURFACEDESC2 sourceInfo = {};
        sourceInfo.dwSize = sizeof(sourceInfo);
        DDSURFACEDESC2 destinationInfo = {};
        destinationInfo.dwSize = sizeof(destinationInfo);
        if (FAILED(source->GetSurfaceDesc(&sourceInfo)) ||
            FAILED(destination->GetSurfaceDesc(&destinationInfo))) return false;

        RECT src = sourceRect ? *sourceRect
                              : RECT{0, 0, (LONG)sourceInfo.dwWidth, (LONG)sourceInfo.dwHeight};
        src.left = std::clamp<LONG>(src.left, 0, (LONG)sourceInfo.dwWidth);
        src.right = std::clamp<LONG>(src.right, 0, (LONG)sourceInfo.dwWidth);
        src.top = std::clamp<LONG>(src.top, 0, (LONG)sourceInfo.dwHeight);
        src.bottom = std::clamp<LONG>(src.bottom, 0, (LONG)sourceInfo.dwHeight);
        if (src.right <= src.left || src.bottom <= src.top ||
            x >= destinationInfo.dwWidth || y >= destinationInfo.dwHeight) return false;
        uint32_t width = std::min<uint32_t>(src.right - src.left, destinationInfo.dwWidth - x);
        uint32_t height = std::min<uint32_t>(src.bottom - src.top, destinationInfo.dwHeight - y);
        src.right = src.left + width;
        src.bottom = src.top + height;
        RECT dst = {(LONG)x, (LONG)y, (LONG)(x + width), (LONG)(y + height)};

        DDCOLORKEY key = {};
        if (FAILED(source->GetColorKey(DDCKEY_SRCBLT, &key))) return false;
        DDSURFACEDESC2 sourceLock = {};
        sourceLock.dwSize = sizeof(sourceLock);
        if (FAILED(source->Lock(&src, &sourceLock, DDLOCK_READONLY | DDLOCK_WAIT, nullptr)))
            return false;
        const bool source32 = sourceLock.lpSurface && sourceLock.lPitch >= 0 &&
                              sourceLock.ddpfPixelFormat.dwRGBBitCount == 32 &&
                              sourceLock.ddpfPixelFormat.dwBBitMask == 0x000000FF &&
                              sourceLock.ddpfPixelFormat.dwGBitMask == 0x0000FF00 &&
                              sourceLock.ddpfPixelFormat.dwRBitMask == 0x00FF0000;
        if (!source32) {
            source->Unlock(&src);
            return false;
        }
        cursor.pixels.resize(static_cast<size_t>(width) * height * 4);
        const uint32_t rgbMask = sourceLock.ddpfPixelFormat.dwRBitMask |
                                 sourceLock.ddpfPixelFormat.dwGBitMask |
                                 sourceLock.ddpfPixelFormat.dwBBitMask;
        const uint32_t keyLow = key.dwColorSpaceLowValue & rgbMask;
        const uint32_t keyHigh = key.dwColorSpaceHighValue & rgbMask;
        for (uint32_t row = 0; row < height; ++row) {
            const auto *srcPixels = reinterpret_cast<const uint32_t *>(
                static_cast<const uint8_t *>(sourceLock.lpSurface) +
                static_cast<size_t>(row) * sourceLock.lPitch);
            auto *out = cursor.pixels.data() + static_cast<size_t>(row) * width * 4;
            for (uint32_t column = 0; column < width; ++column) {
                const uint32_t raw = srcPixels[column];
                const uint32_t rgb = raw & rgbMask;
                std::memcpy(out + column * 4, &raw, 3);
                out[column * 4 + 3] = rgb >= keyLow && rgb <= keyHigh ? 0 : 0xFF;
            }
        }
        source->Unlock(&src);

        DDSURFACEDESC2 destinationLock = {};
        destinationLock.dwSize = sizeof(destinationLock);
        if (FAILED(destination->Lock(&dst, &destinationLock,
                                     DDLOCK_READONLY | DDLOCK_WAIT, nullptr))) return false;
        const bool destination32 = destinationLock.lpSurface && destinationLock.lPitch >= 0 &&
                                   destinationLock.ddpfPixelFormat.dwRGBBitCount == 32;
        if (!destination32) {
            destination->Unlock(&dst);
            return false;
        }
        cursor.background.resize(static_cast<size_t>(width) * height * 4);
        for (uint32_t row = 0; row < height; ++row) {
            uint8_t *out = cursor.background.data() + static_cast<size_t>(row) * width * 4;
            std::memcpy(out, static_cast<const uint8_t *>(destinationLock.lpSurface) +
                             static_cast<size_t>(row) * destinationLock.lPitch,
                        static_cast<size_t>(width) * 4);
            for (uint32_t column = 0; column < width; ++column) out[column * 4 + 3] = 0xFF;
        }
        destination->Unlock(&dst);
        cursor.rect = dst;
        cursor.width = width;
        cursor.height = height;
        cursor.visible = true;
        return true;
    }

    static void stripCursor(TextureCache &surface, const CursorSnapshot &cursor,
                            bool matteWhite) {
        if (!cursor.visible || cursor.matteWhite != matteWhite || cursor.pixels.empty() ||
            cursor.background.size() != cursor.pixels.size()) return;
        const LONG left = std::clamp<LONG>(cursor.rect.left, 0, (LONG)surface.width);
        const LONG top = std::clamp<LONG>(cursor.rect.top, 0, (LONG)surface.height);
        const LONG right = std::clamp<LONG>(cursor.rect.right, left, (LONG)surface.width);
        const LONG bottom = std::clamp<LONG>(cursor.rect.bottom, top, (LONG)surface.height);
        for (LONG y = top; y < bottom; ++y) {
            const uint32_t sourceY = (uint32_t)(y - cursor.rect.top);
            for (LONG x = left; x < right; ++x) {
                const uint32_t sourceX = (uint32_t)(x - cursor.rect.left);
                const size_t cursorOffset = (static_cast<size_t>(sourceY) * cursor.width + sourceX) * 4;
                if (cursor.pixels[cursorOffset + 3] == 0) continue;
                uint8_t *pixel = surface.pixels.data() + static_cast<size_t>(y) * surface.rowPitch + x * 4;
                if (std::memcmp(pixel, cursor.pixels.data() + cursorOffset, 3) == 0)
                    std::memcpy(pixel, cursor.background.data() + cursorOffset, 4);
            }
        }
    }

    void markOverlayRect(const RECT &rect) {
        if (!overlayTileColumns_ || !overlayTileRows_) return;
        const uint32_t left = (uint32_t)std::clamp<LONG>(rect.left, 0, (LONG)overlay_.width);
        const uint32_t right = (uint32_t)std::clamp<LONG>(rect.right, 0, (LONG)overlay_.width);
        const uint32_t top = (uint32_t)std::clamp<LONG>(rect.top, 0, (LONG)overlay_.height);
        const uint32_t bottom = (uint32_t)std::clamp<LONG>(rect.bottom, 0, (LONG)overlay_.height);
        if (right <= left || bottom <= top) return;
        const uint32_t firstTileX = left / kOverlayTileSize;
        const uint32_t lastTileX = (right - 1) / kOverlayTileSize;
        const uint32_t firstTileY = top / kOverlayTileSize;
        const uint32_t lastTileY = (bottom - 1) / kOverlayTileSize;
        for (uint32_t tileY = firstTileY; tileY <= lastTileY; ++tileY)
            for (uint32_t tileX = firstTileX; tileX <= lastTileX; ++tileX)
                overlayChanged_[static_cast<size_t>(tileY) * overlayTileColumns_ + tileX] = 1;
    }

    void restoreCompositedCursor() {
        const uint32_t width = (uint32_t)std::max<LONG>(0,
            compositedCursorRect_.right - compositedCursorRect_.left);
        const uint32_t height = (uint32_t)std::max<LONG>(0,
            compositedCursorRect_.bottom - compositedCursorRect_.top);
        if (!width || !height || cursorUnderlay_.size() != static_cast<size_t>(width) * height * 4)
            return;
        for (uint32_t row = 0; row < height; ++row)
            std::memcpy(overlay_.pixels.data() +
                            static_cast<size_t>(compositedCursorRect_.top + row) * overlay_.rowPitch +
                            compositedCursorRect_.left * 4,
                        cursorUnderlay_.data() + static_cast<size_t>(row) * width * 4,
                        static_cast<size_t>(width) * 4);
        markOverlayRect(compositedCursorRect_);
        compositedCursorRect_ = {};
        cursorUnderlay_.clear();
    }

    void compositeCursor(const CursorSnapshot &cursor) {
        if (!cursor.visible || cursor.pixels.empty()) return;
        RECT clipped = {
            std::clamp<LONG>(cursor.rect.left, 0, (LONG)overlay_.width),
            std::clamp<LONG>(cursor.rect.top, 0, (LONG)overlay_.height),
            std::clamp<LONG>(cursor.rect.right, 0, (LONG)overlay_.width),
            std::clamp<LONG>(cursor.rect.bottom, 0, (LONG)overlay_.height)};
        if (clipped.right <= clipped.left || clipped.bottom <= clipped.top) return;
        const uint32_t width = clipped.right - clipped.left;
        const uint32_t height = clipped.bottom - clipped.top;
        cursorUnderlay_.resize(static_cast<size_t>(width) * height * 4);
        for (uint32_t row = 0; row < height; ++row) {
            uint8_t *destination = overlay_.pixels.data() +
                                   static_cast<size_t>(clipped.top + row) * overlay_.rowPitch +
                                   clipped.left * 4;
            std::memcpy(cursorUnderlay_.data() + static_cast<size_t>(row) * width * 4,
                        destination, static_cast<size_t>(width) * 4);
            const uint32_t sourceY = clipped.top + row - cursor.rect.top;
            for (uint32_t column = 0; column < width; ++column) {
                const uint32_t sourceX = clipped.left + column - cursor.rect.left;
                const uint8_t *source = cursor.pixels.data() +
                    (static_cast<size_t>(sourceY) * cursor.width + sourceX) * 4;
                if (source[3]) std::memcpy(destination + column * 4, source, 4);
            }
        }
        compositedCursorRect_ = clipped;
        markOverlayRect(clipped);
    }

    void updateOverlay(const TextureCache &black, const TextureCache &white, RECT process,
                       const CursorSnapshot &cursor) {
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
            process = {0, 0, (LONG)black.width, (LONG)black.height};
        }

        const uint32_t xBegin = (uint32_t)std::clamp<LONG>(process.left, 0, (LONG)black.width);
        const uint32_t xEnd = (uint32_t)std::clamp<LONG>(process.right, 0, (LONG)black.width);
        const uint32_t yBegin = (uint32_t)std::clamp<LONG>(process.top, 0, (LONG)black.height);
        const uint32_t yEnd = (uint32_t)std::clamp<LONG>(process.bottom, 0, (LONG)black.height);
        std::fill(overlayChanged_.begin(), overlayChanged_.end(), 0);
        restoreCompositedCursor();
        for (uint32_t y = yBegin; y < yEnd; ++y) {
            const uint8_t *blackRow = black.pixels.data() + static_cast<size_t>(y) * black.rowPitch;
            const uint8_t *whiteRow = white.pixels.data() + static_cast<size_t>(y) * white.rowPitch;
            uint8_t *overlayRow = overlay_.pixels.data() + static_cast<size_t>(y) * overlay_.rowPitch;
            unmatteOverlaySpan(blackRow + static_cast<size_t>(xBegin) * 4,
                               whiteRow + static_cast<size_t>(xBegin) * 4,
                               overlayLine_.data() + static_cast<size_t>(xBegin) * 4,
                               xEnd - xBegin);
            const uint32_t tileY = y / kOverlayTileSize;
            const uint32_t firstTileX = xBegin / kOverlayTileSize;
            const uint32_t endTileX = (xEnd + kOverlayTileSize - 1) / kOverlayTileSize;
            for (uint32_t tileX = firstTileX; tileX < endTileX; ++tileX) {
                const uint32_t tileLeft = tileX * kOverlayTileSize;
                const uint32_t copyLeft = std::max(tileLeft, xBegin);
                const uint32_t copyRight = std::min(tileLeft + kOverlayTileSize, xEnd);
                const size_t bytes = static_cast<size_t>(copyRight - copyLeft) * 4;
                if (std::memcmp(overlayRow + static_cast<size_t>(copyLeft) * 4,
                                overlayLine_.data() + static_cast<size_t>(copyLeft) * 4,
                                bytes) == 0) continue;
                std::memcpy(overlayRow + static_cast<size_t>(copyLeft) * 4,
                            overlayLine_.data() + static_cast<size_t>(copyLeft) * 4, bytes);
                overlayChanged_[static_cast<size_t>(tileY) * overlayTileColumns_ + tileX] = 1;
            }
        }
        compositeCursor(cursor);
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
    TextureCache parityCache_[2];
    std::mutex overlayMutex_;
    DWORD frameThreadId_ = 0;
    CursorSnapshot cursor_;
    bool cursorCaptureLogged_ = false;
    bool cursorCaptureFailureLogged_ = false;
    RECT compositedCursorRect_ = {};
    std::vector<uint8_t> cursorUnderlay_;
    RECT parityDrawn_[2] = {};
    RECT parityRefresh_[2] = {};
    RECT drawnRect_ = {};
    bool drawnValid_ = false;
    bool drawnFull_ = false;
    bool clearedThisFrame_ = false;
    std::vector<OverlayTileState> overlayTiles_;
    std::vector<uint8_t> overlayChanged_;
    std::vector<uint8_t> overlayLine_;
    uint32_t overlayTileColumns_ = 0;
    uint32_t overlayTileRows_ = 0;
    uint32_t overlayConsumerSession_ = 0;
    uint32_t overlayLastSentFrame_ = 0;
    bool overlayForceFull_ = true;
    bool overlayWhite_ = false;
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

void overlayCleared() { producer.overlayCleared(); }

void overlayDrawn(const RECT *rect) { producer.overlayDrawn(rect); }

void overlayBltFast(IDirectDrawSurface4 *destination, DWORD x, DWORD y,
                    IDirectDrawSurface4 *source, const RECT *sourceRect,
                    DWORD flags) {
    producer.overlayBltFast(destination, x, y, source, sourceRect, flags);
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
