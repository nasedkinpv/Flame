#include <metal_bridge/MetalBridgeProducer.h>
#include <metal_bridge/DK2BridgeProtocol.h>
#include <metal_bridge/OverlayUnmatte.h>
#include <dk2/ShadowGpu.h>
#include <gog_globals.h>
#include <gog_debug.h>
#include <dk2_functions.h>
#include <patches/replace_mouse_dinput_to_user32.h>
#include <d3d.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    // generation of parityCache_[0]/[1] this tile was last unmatted against;
    // 0 means "never processed" and never matches a real touched generation
    // (markTilesTouched skips 0 on wraparound), so a freshly-sized tile array
    // is naturally dirty everywhere on its first pass.
    uint32_t lastBlackGen = 0;
    uint32_t lastWhiteGen = 0;
};

struct CursorSnapshot {
    RECT rect = {};
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;
    int32_t mouseX = 0;
    int32_t mouseY = 0;
    int32_t hotspotX = 0;
    int32_t hotspotY = 0;
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
        const bool freshConsumer =
            consumerSession != lastConsumerSession_ || consumer < lastConsumerFrame_;
        if (freshConsumer) {
            for (auto &entry : textures_) entry.second.pending = true;
            // A fresh consumer needs the full atlas map again. Unlike draw
            // state, maps are persistent host state and must not depend on a
            // one-shot frame surviving the three-slot mailbox.
            atlasRectsAcked_ = 0;
            atlasRectsEmitted_ = 0;
            atlasLastSentFrame_ = 0;
            // A new host starts with no texture cache, so releases intended
            // for the previous host are already satisfied.
            textureReleases_.clear();
            textureReleasesAcked_ = 0;
            textureReleasesEmitted_ = 0;
            textureReleaseLastSentFrame_ = 0;
            // Same for atlas resets: a fresh host holds no page maps yet.
            atlasResets_.clear();
            atlasResetsAcked_ = 0;
            atlasResetsEmitted_ = 0;
            atlasResetLastSentFrame_ = 0;
            for (auto &entry : shadowMasks_) {
                entry.second.lastSentGeneration = entry.second.generation;
            }
        } else {
            if (atlasLastSentFrame_) {
                if (consumer == atlasLastSentFrame_) {
                    atlasRectsAcked_ = atlasRectsEmitted_;
                } else {
                    // The consumer skipped (or has not yet accepted) the frame
                    // carrying these maps. Retry from the last acknowledged map.
                    atlasRectsEmitted_ = atlasRectsAcked_;
                }
                atlasLastSentFrame_ = 0;
            }
            if (textureReleaseLastSentFrame_) {
                if (consumer == textureReleaseLastSentFrame_) {
                    textureReleasesAcked_ = textureReleasesEmitted_;
                } else {
                    textureReleasesEmitted_ = textureReleasesAcked_;
                }
                textureReleaseLastSentFrame_ = 0;
            }
            if (textureReleasesAcked_ == textureReleases_.size() &&
                !textureReleases_.empty()) {
                textureReleases_.clear();
                textureReleasesAcked_ = 0;
                textureReleasesEmitted_ = 0;
            }
            if (atlasResetLastSentFrame_) {
                if (consumer == atlasResetLastSentFrame_) {
                    atlasResetsAcked_ = atlasResetsEmitted_;
                } else {
                    atlasResetsEmitted_ = atlasResetsAcked_;
                }
                atlasResetLastSentFrame_ = 0;
            }
            if (atlasResetsAcked_ == atlasResets_.size() && !atlasResets_.empty()) {
                atlasResets_.clear();
                atlasResetsAcked_ = 0;
                atlasResetsEmitted_ = 0;
            }
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

        emitTextureReleases();

        // Maps precede full texture uploads. The host also pre-scans maps,
        // but keeping the stream ordered makes captures and other consumers
        // self-contained without relying on a second pass. Resets must land
        // before the rects that describe the page's new layout.
        emitAtlasResets();
        emitAtlasRects();

        for (DWORD stage = 0; stage < 3; ++stage) {
            if (boundTextures_[stage]) {
                emitTexture(stage, boundTextures_[stage], boundSurfaces_[stage]);
            }
        }
        flushPendingLights();
        // Mesh commands staged during the game's prepare phase (sceneEmit runs
        // before BeginScene, i.e. between frames) flush into the frame head:
        // they draw first, and z-testing orders them against later draws.
        if (!stagedMesh_.empty()) {
            for (uint32_t id : stagedMeshTextures_) emitTexture(0, id);
            if (used_ + stagedMesh_.size() <= DK2M_SLOT_CAPACITY) {
                append(stagedMesh_.data(), static_cast<uint32_t>(stagedMesh_.size()));
                commandCount_ += stagedMeshCommandCount_;
            }
            stagedMesh_.clear();
            stagedMeshTextures_.clear();
            stagedMeshCommandCount_ = 0;
            stagedCamera_ = false;
            stagedLights_ = false;
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
        boundSurfaces_[stage] = surface;
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
            if (surface) {
                surfaceTextures_[reinterpret_cast<uintptr_t>(surface)] = textureId;
                flushPendingAtlasRects(reinterpret_cast<uintptr_t>(surface), textureId);
            }
        }
        if (active_) emitTexture(stage, textureId, surface);
    }

    // Assigns (and captures) a bridge texture id for a raw surface that never
    // went through the legacy SetTexture path - terrain cache pages under the
    // GPU mesh path never get a lazily-created FakeTexture, so their id comes
    // from this synthetic namespace instead.
    uint32_t ensureSurfaceTexture(IDirectDrawSurface4 *surface) {
        if (!surface || !ensureMapped()) return 0;
        const auto found = surfaceTextures_.find(reinterpret_cast<uintptr_t>(surface));
        uint32_t textureId;
        if (found != surfaceTextures_.end()) {
            textureId = found->second;
        } else {
            textureId = nextSyntheticTextureId_++;
        }
        ensureTexture(textureId, surface);
        return textures_.count(textureId) ? textureId : 0;
    }

    // Registers a raw BGRA32 CPU buffer (an engine CEngineSurface page) under
    // a stable synthetic id keyed by the surface object pointer. Alpha is
    // forced opaque - atlas pages carry colour, not coverage.
    uint32_t ensureBufferTexture(const void *key, const void *pixels,
                                 uint32_t width, uint32_t height, uint32_t pitchBytes) {
        if (!key || !pixels || !width || !height || width > 4096 || height > 4096 ||
            pitchBytes < width * 4 || !ensureMapped()) return 0;
        const auto found = surfaceTextures_.find(reinterpret_cast<uintptr_t>(key));
        uint32_t textureId;
        if (found != surfaceTextures_.end()) {
            textureId = found->second;
        } else {
            textureId = nextSyntheticTextureId_++;
            surfaceTextures_[reinterpret_cast<uintptr_t>(key)] = textureId;
            flushPendingAtlasRects(reinterpret_cast<uintptr_t>(key), textureId);
        }
        auto existing = textures_.find(textureId);
        if (existing != textures_.end() && !existing->second.dirty) return textureId;
        TextureCache cache;
        cache.width = width;
        cache.height = height;
        cache.rowPitch = width * 4;
        cache.pixels.resize(static_cast<size_t>(cache.rowPitch) * height);
        const auto *src = static_cast<const uint8_t *>(pixels);
        for (uint32_t y = 0; y < height; ++y) {
            uint8_t *dst = cache.pixels.data() + static_cast<size_t>(y) * cache.rowPitch;
            std::memcpy(dst, src + static_cast<size_t>(y) * pitchBytes, cache.rowPitch);
            for (uint32_t x = 0; x < width; ++x) dst[x * 4 + 3] = 0xFF;
        }
        if (existing != textures_.end()) {
            updateTexture(existing->second, std::move(cache));
        } else {
            textures_.emplace(textureId, std::move(cache));
        }
        return textureId;
    }

    bool bufferTextureNeedsRefresh(uint32_t textureId) const {
        const auto found = textures_.find(textureId);
        return found == textures_.end() || found->second.dirty;
    }

    void surfaceReleased(const void *key) {
        if (!key) return;
        const uintptr_t pageKey = reinterpret_cast<uintptr_t>(key);
        const auto found = surfaceTextures_.find(pageKey);
        if (found == surfaceTextures_.end()) {
            // An unresolved atlas report is the only state an unregistered
            // surface can own. Most CEngineSurface instances are temporary
            // decode buffers, so keep their destructor path O(1).
            if (pendingAtlasRects_.find(pageKey) != pendingAtlasRects_.end()) {
                forgetSurfaceKey(pageKey);
            }
            return;
        }
        textureReleased(found->second, key);
    }

    void textureReleased(uint32_t textureId, const void *key) {
        if (!textureId) return;

        bool wasKnown = false;
        std::vector<uintptr_t> pageKeys;
        if (key) pageKeys.push_back(reinterpret_cast<uintptr_t>(key));
        for (auto it = surfaceTextures_.begin(); it != surfaceTextures_.end();) {
            if (it->second != textureId) {
                ++it;
                continue;
            }
            if (std::find(pageKeys.begin(), pageKeys.end(), it->first) == pageKeys.end()) {
                pageKeys.push_back(it->first);
            }
            wasKnown = true;
            it = surfaceTextures_.erase(it);
        }
        for (uintptr_t pageKey : pageKeys) forgetSurfaceKey(pageKey);

        wasKnown = textures_.erase(textureId) != 0 || wasKnown;
        for (DWORD stage = 0; stage < 3; ++stage) {
            if (boundTextures_[stage] != textureId) continue;
            boundTextures_[stage] = 0;
            boundSurfaces_[stage] = nullptr;
        }

        const size_t oldAtlasSize = atlasRects_.size();
        atlasRects_.erase(
            std::remove_if(atlasRects_.begin(), atlasRects_.end(),
                           [textureId](const DK2MPageAtlasMapCommand &rect) {
                               return rect.textureId == textureId;
                           }),
            atlasRects_.end());
        if (atlasRects_.size() != oldAtlasSize) {
            wasKnown = true;
            // Indices are the reliability watermark. Compacting invalidates
            // them, so replay the remaining live map from zero.
            atlasRectsAcked_ = 0;
            atlasRectsEmitted_ = 0;
            atlasLastSentFrame_ = 0;
        }

        if (!wasKnown) return;
        if ((++textureReleaseCount_ % 250) == 1) {
            gog_debugf("Metal bridge: texture releases=%u live=%u atlas-rects=%u",
                       textureReleaseCount_, static_cast<unsigned>(textures_.size()),
                       static_cast<unsigned>(atlasRects_.size()));
        }
        textureReleases_.push_back(textureId);
        if (active_) emitTextureReleases();
    }

    // --- named-atlas map (HD resource pack) ---
    // Composition points report (page surface, resource name, rect). The
    // page's bridge texture id may not exist yet (compositing precedes the
    // first upload), so unresolved reports wait in pendingAtlasRects_ keyed
    // by the page pointer and flush when any of the id-assignment sites
    // below first associates that pointer with an id. Resolved rects live
    // in atlasRects_ forever and are (re)emitted from a watermark, so a
    // restarted consumer receives the full map again.
    void reportAtlasRect(const void *pageKey, const char *rawName,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        ++atlasReportCalls_;
        if (!pageKey || !rawName || !*rawName || !w || !h ||
            x > 0xFFFF || y > 0xFFFF || w > 0xFFFF || h > 0xFFFF) {
            ++atlasRejectedInvalid_;
            return;
        }
        // Every engine reduction level is a valid atlas page. The resource
        // pack contains one canonical base image, so FooMM0..FooMM9 all map
        // to Foo; the host scales it to the selected rect and builds mips.
        // Millions of calls per session hit this path: stay allocation-free
        // until the per-page dedupe check has passed.
        size_t n = std::strlen(rawName);
        if (n >= 3 && rawName[n - 3] == 'M' && rawName[n - 2] == 'M' &&
            rawName[n - 1] >= '0' && rawName[n - 1] <= '9') {
            n -= 3;
            ++atlasMipCanonicalized_;
            if (!n) return;
        }
        uint64_t rectKey = 1469598103934665603ull;  // FNV-1a over (x, y, name)
        auto mix8 = [&rectKey](uint8_t b) { rectKey ^= b; rectKey *= 1099511628211ull; };
        mix8(static_cast<uint8_t>(x)); mix8(static_cast<uint8_t>(x >> 8));
        mix8(static_cast<uint8_t>(y)); mix8(static_cast<uint8_t>(y >> 8));
        for (size_t i = 0; i < n; ++i) mix8(static_cast<uint8_t>(rawName[i]));
        if (!atlasSeenByPage_[reinterpret_cast<uintptr_t>(pageKey)]
                 .insert(rectKey).second) return;
        std::string name(rawName, n);
        if ((++atlasReported_ % 250) == 1) {
            size_t pendingCount = 0;
            for (const auto &kv : pendingAtlasRects_) pendingCount += kv.second.size();
            gog_debugf("atlas-map: calls=%u unique=%u mip-canonicalized=%u invalid=%u "
                       "resolved=%u pending=%u acked=%u emitted=%u",
                       atlasReportCalls_, atlasReported_, atlasMipCanonicalized_,
                       atlasRejectedInvalid_, static_cast<unsigned>(atlasRects_.size()),
                       static_cast<unsigned>(pendingCount),
                       static_cast<unsigned>(atlasRectsAcked_),
                       static_cast<unsigned>(atlasRectsEmitted_));
        }
        const auto found = surfaceTextures_.find(reinterpret_cast<uintptr_t>(pageKey));
        if (found != surfaceTextures_.end()) {
            pushAtlasRect(found->second, name.c_str(), x, y, w, h);
        } else {
            PendingAtlasRect pending{};
            pending.x = static_cast<uint16_t>(x);
            pending.y = static_cast<uint16_t>(y);
            pending.w = static_cast<uint16_t>(w);
            pending.h = static_cast<uint16_t>(h);
            std::strncpy(pending.name, name.c_str(), sizeof(pending.name) - 1);
            pendingAtlasRects_[reinterpret_cast<uintptr_t>(pageKey)].push_back(pending);
        }
    }

    void flushPendingAtlasRects(uintptr_t pageKey, uint32_t textureId) {
        const auto found = pendingAtlasRects_.find(pageKey);
        if (found == pendingAtlasRects_.end()) return;
        for (const PendingAtlasRect &p : found->second) {
            pushAtlasRect(textureId, p.name, p.x, p.y, p.w, p.h);
        }
        pendingAtlasRects_.erase(found);
    }

    void pushAtlasRect(uint32_t textureId, const char *name,
                       uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        DK2MPageAtlasMapCommand cmd = {};
        cmd.textureId = textureId;
        cmd.x = static_cast<uint16_t>(x);
        cmd.y = static_cast<uint16_t>(y);
        cmd.w = static_cast<uint16_t>(w);
        cmd.h = static_cast<uint16_t>(h);
        std::strncpy(cmd.name, name, sizeof(cmd.name) - 1);
        atlasRects_.push_back(cmd);
        // If this page was uploaded before its first map existed, force one
        // acknowledged full upload so the host has the original 1x backing
        // pixels from which to compose the named HD atlas.
        const auto texture = textures_.find(textureId);
        if (texture != textures_.end()) {
            texture->second.pending = true;
            texture->second.lastSentFrame = 0;
        }
        // mid-frame reports still reach the consumer this frame
        if (active_) emitAtlasRects();
    }

    void emitAtlasRects() {
        while (atlasRectsEmitted_ < atlasRects_.size()) {
            struct {
                DK2MCommandHeader header;
                DK2MPageAtlasMapCommand body;
            } cmd = {};
            cmd.header.type = DK2M_COMMAND_PAGE_ATLAS_MAP;
            cmd.header.size = sizeof(cmd);
            cmd.body = atlasRects_[atlasRectsEmitted_];
            if (used_ + sizeof(cmd) > DK2M_SLOT_CAPACITY) break;
            append(&cmd, sizeof(cmd));
            ++commandCount_;
            ++atlasRectsEmitted_;
        }
    }

    // Engine repacked this atlas page (SurfHashList2 detach + recomposite):
    // drop everything we believe about its layout and tell the host to do
    // the same. Placements reported afterwards describe the new layout.
    void atlasPageReset(const void *pageKey) {
        if (!pageKey) return;
        const uintptr_t key = reinterpret_cast<uintptr_t>(pageKey);
        atlasSeenByPage_.erase(key);
        pendingAtlasRects_.erase(key);
        const auto found = surfaceTextures_.find(key);
        if (found == surfaceTextures_.end()) return;
        const uint32_t textureId = found->second;
        if (const auto texture = textures_.find(textureId);
            texture != textures_.end()) {
            texture->second.dirty = true;
        }
        const size_t oldSize = atlasRects_.size();
        atlasRects_.erase(
            std::remove_if(atlasRects_.begin(), atlasRects_.end(),
                           [textureId](const DK2MPageAtlasMapCommand &rect) {
                               return rect.textureId == textureId;
                           }),
            atlasRects_.end());
        if (atlasRects_.size() != oldSize) {
            // Watermark indices shifted; replay the live map from zero.
            atlasRectsAcked_ = 0;
            atlasRectsEmitted_ = 0;
            atlasLastSentFrame_ = 0;
        }
        atlasResets_.push_back(textureId);
        if ((++atlasResetCount_ % 100) == 1) {
            gog_debugf("atlas-map: page resets=%u live-rects=%u",
                       atlasResetCount_, static_cast<unsigned>(atlasRects_.size()));
        }
        if (active_) emitAtlasResets();
    }

    void emitAtlasResets() {
        while (atlasResetsEmitted_ < atlasResets_.size()) {
            DK2MPageAtlasResetCommand command = {};
            command.header.type = DK2M_COMMAND_PAGE_ATLAS_RESET;
            command.header.size = sizeof(command);
            command.texture_id = atlasResets_[atlasResetsEmitted_];
            if (used_ + sizeof(command) > DK2M_SLOT_CAPACITY) break;
            append(&command, sizeof(command));
            ++commandCount_;
            ++atlasResetsEmitted_;
        }
    }

    void emitTextureReleases() {
        while (textureReleasesEmitted_ < textureReleases_.size()) {
            DK2MTextureReleaseCommand command = {};
            command.header.type = DK2M_COMMAND_TEXTURE_RELEASE;
            command.header.size = sizeof(command);
            command.texture_id = textureReleases_[textureReleasesEmitted_];
            if (used_ + sizeof(command) > DK2M_SLOT_CAPACITY) break;
            append(&command, sizeof(command));
            ++commandCount_;
            ++textureReleasesEmitted_;
        }
    }

    bool metalShadowsEnabled() const {
        return metalShadowsEnabled_.load(std::memory_order_relaxed);
    }

    void shadowMask(const void *handleKey, const void *triangles,
                    uint32_t triangleCount, uint32_t mode) {
        if (!handleKey || !metalShadowsEnabled() ||
            (triangleCount && !triangles)) return;
        const uint64_t triangleBytes =
            static_cast<uint64_t>(triangleCount) * sizeof(DK2MShadowTriangle);
        if (triangleBytes > DK2M_SLOT_CAPACITY - sizeof(DK2MShadowMaskCommand)) return;
        ShadowMaskState &state = shadowMasks_[reinterpret_cast<uintptr_t>(handleKey)];
        state.triangles.resize(triangleCount);
        if (triangleCount) {
            std::memcpy(state.triangles.data(), triangles,
                        static_cast<size_t>(triangleBytes));
        }
        state.mode = mode;
        if (++state.generation == 0) ++state.generation;
    }

    // Capture-only registration for the GPU mesh path: same cache upkeep as
    // texture(), but never touches bound stage state and never emits a
    // SET_TEXTURE - mesh draws carry their texture id in the command itself.
    void ensureTexture(DWORD textureId, IDirectDrawSurface4 *surface) {
        if (!textureId || !ensureMapped()) return;
        auto found = textures_.find(textureId);
        if (found == textures_.end()) {
            TextureCache cache;
            if (!captureTexture(surface, cache)) {
                static bool loggedCaptureFailure = false;
                if (!loggedCaptureFailure) {
                    loggedCaptureFailure = true;
                    gog_debugf("Metal bridge: mesh texture %u capture failed (surface=%p)",
                               textureId, surface);
                }
                return;
            }
            textures_.emplace(textureId, std::move(cache));
        } else if (found->second.dirty) {
            TextureCache updated;
            if (captureTexture(surface, updated)) {
                updateTexture(found->second, std::move(updated));
            }
        }
        if (surface) {
            surfaceTextures_[reinterpret_cast<uintptr_t>(surface)] = textureId;
            flushPendingAtlasRects(reinterpret_cast<uintptr_t>(surface), textureId);
        }
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
        flushPendingLights();
        const auto overlayStarted = PhaseClock::now();
        emitOverlay();
        emitCursor();
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
        if (atlasRectsEmitted_ > atlasRectsAcked_) atlasLastSentFrame_ = frame_;
        if (textureReleasesEmitted_ > textureReleasesAcked_) {
            textureReleaseLastSentFrame_ = frame_;
        }
        if (atlasResetsEmitted_ > atlasResetsAcked_) atlasResetLastSentFrame_ = frame_;
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

    bool cursor(IDirectDrawSurface *source, DWORD width, DWORD height, DWORD colorKey,
                LONG mouseX, LONG mouseY, LONG hotspotX, LONG hotspotY) {
        if (!source || !width || !height) return false;
        IDirectDrawSurface4 *surface4 = nullptr;
        if (FAILED(source->QueryInterface(IID_IDirectDrawSurface4,
                                          reinterpret_cast<void **>(&surface4))) ||
            !surface4) return false;
        CursorSnapshot captured;
        const bool ok = captureNativeCursor(surface4, width, height, colorKey, captured);
        surface4->Release();
        if (!ok) return false;

        captured.mouseX = mouseX;
        captured.mouseY = mouseY;
        captured.hotspotX = hotspotX;
        captured.hotspotY = hotspotY;
        // The overlay-space rect (strip/tooltip region) lives entirely in
        // captured (game-resolution) coordinates: build it from the raw
        // hotspot BEFORE normalizeCursorSize rescales the hotspot to the
        // logical display size, or the erase region drifts.
        captured.rect = {mouseX - hotspotX, mouseY - hotspotY,
                         mouseX - hotspotX + static_cast<LONG>(width),
                         mouseY - hotspotY + static_cast<LONG>(height)};
        // Draw at the full captured (game-resolution) size. The cursor is the
        // 3D-rendered Hand of Evil, already rasterized at game scale - the
        // old normalizeCursorSize() shrink to the 640x480 logical size threw
        // that detail away AND desynced the game's tooltip layout, which is
        // computed against the full-size cursor (the 2.5x tooltip gap).
        captured.visible = true;

        const uint64_t geometry = static_cast<uint64_t>(captured.width) |
                                  (static_cast<uint64_t>(captured.height) << 16) |
                                  (static_cast<uint64_t>(captured.displayWidth) << 32) |
                                  (static_cast<uint64_t>(captured.displayHeight) << 48);
        std::lock_guard<std::mutex> lock(overlayMutex_);
        if (geometry != lastCursorGeometry_) {
            size_t transparent = 0;
            for (size_t offset = 3; offset < captured.pixels.size(); offset += 4)
                if (captured.pixels[offset] == 0) ++transparent;
            gog_debugf("Metal cursor: source %ux%u, logical %ux%u, hotspot %d,%d, key=0x%x transparent=%u/%u",
                       captured.width, captured.height,
                       captured.displayWidth, captured.displayHeight,
                       captured.hotspotX, captured.hotspotY, colorKey,
                       static_cast<unsigned>(transparent),
                       captured.width * captured.height);
            lastCursorGeometry_ = geometry;
        }
        cursor_ = std::move(captured);
        return true;
    }

    void hideCursor() {
        std::lock_guard<std::mutex> lock(overlayMutex_);
        cursor_.visible = false;
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

        RECT touched;  // the sub-rect of cache.pixels actually written this call
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
                touched = {0, 0, (LONG)cache.width, (LONG)cache.height};
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
                touched = refresh;
            }
        } else {
            if (!captureTexture(surface, cache)) return;
            parityDrawn_[parity] = {0, 0, (LONG)cache.width, (LONG)cache.height};
            touched = {0, 0, (LONG)cache.width, (LONG)cache.height};
        }
        markTilesTouched(parityTileGen_[parity], touched);
        if (cursor.visible) {
            // stripCursor only actually edits pixels when cursor.matteWhite
            // matches this parity and geometry lines up, but marking the
            // rect unconditionally is cheap and keeps this correct even if
            // stripCursor's own guard changes later.
            markTilesTouched(parityTileGen_[parity], cursor.rect);
        }
        stripCursor(cache, cursor, parity != 0);

        TextureCache &other = parityCache_[1 - parity];
        if (!other.pixels.empty() && other.width == cache.width &&
            other.height == cache.height && other.rowPitch == cache.rowPitch) {
            updateOverlay(parityCache_[0], parityCache_[1], cursor);
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

    void setMetalShadowsEnabled(bool enabled) {
        const bool previous = metalShadowsEnabled_.exchange(enabled, std::memory_order_relaxed);
        if (previous == enabled) return;
        gog_debugf("metal shadows bridge: GPU rasterizer %s", enabled ? "on" : "off");
        if (!enabled) {
            for (auto &entry : shadowMasks_) {
                entry.second.lastSentGeneration = entry.second.generation;
            }
        }
    }

    void processInput() {
        DK2MInputState input = {};
        if (!readInput(input)) return;
        if (input.host_pid == 0) {
            setMetalShadowsEnabled(false);
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
            setMetalShadowsEnabled(false);
            releaseAppliedInput();
            inputHostPid_ = 0;
            return;
        }

        const bool active = (input.flags & DK2M_INPUT_ACTIVE) != 0;
        setMetalShadowsEnabled((input.flags & DK2M_INPUT_METAL_SHADOWS) != 0);
        if (!newHost && active) {
            const uint32_t eventCount = input.event_write - inputEventWrite_;
            if (eventCount <= DK2M_INPUT_EVENT_CAPACITY) {
                for (uint32_t serial = inputEventWrite_ + 1; serial != input.event_write + 1; ++serial) {
                    const DK2MInputEvent &event =
                        input.events[(serial - 1) % DK2M_INPUT_EVENT_CAPACITY];
                    if (event.type == DK2M_INPUT_EVENT_BUTTON) applyButton(event.code, event.value);
                    else if (event.type == DK2M_INPUT_EVENT_KEY) applyKey(event.code, event.value != 0);
                    else if (event.type == DK2M_INPUT_EVENT_CHAR)
                        dk2::MyInputManagerCb_static_windowMsgW(WM_CHAR, event.value);
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

    // DDPIXELFORMAT is a union: for DDPF_BUMPDUDV/BUMPLUMINANCE surfaces the
    // fields conventionally read as RGB masks instead hold
    // dwBumpDuBitMask/dwBumpDvBitMask/dwBumpLuminanceBitMask (same offsets).
    static constexpr DWORD kDDPF_BUMPLUMINANCE = 0x00040000;
    static constexpr DWORD kDDPF_BUMPDUDV = 0x00080000;

    static int maskShift(DWORD mask) {
        if (!mask) return 0;
        int shift = 0;
        while ((mask & 1u) == 0) { mask >>= 1; ++shift; }
        return shift;
    }

    static int maskBits(DWORD mask) {
        int bits = 0;
        while (mask) { bits += mask & 1u; mask >>= 1; }
        return bits;
    }

    // Signed bump component (Du/Dv, arbitrary bit width) -> unsigned-biased
    // byte, matching the shader's `sample*2-1` convention for recovering the
    // signed [-1,1] range from a plain unorm texture read.
    static uint8_t signedBumpComponentToByte(uint32_t raw, DWORD mask) {
        if (!mask) return 128;
        const int shift = maskShift(mask);
        const int bits = maskBits(mask);
        int32_t value = static_cast<int32_t>((raw & mask) >> shift);
        const int32_t half = 1 << (bits - 1);
        if (value >= half) value -= (half << 1);  // sign-extend
        // ponytail: shift-based rescale to 8 bits, not full bit replication -
        // a few LSBs of precision on a 5-bit bump channel is not visible.
        const int32_t scaled = bits >= 8 ? (value >> (bits - 8)) : (value << (8 - bits));
        return static_cast<uint8_t>(scaled + 128);
    }

    static uint8_t unsignedComponentToByte(uint32_t raw, DWORD mask) {
        if (!mask) return 0;
        const int shift = maskShift(mask);
        const int bits = maskBits(mask);
        const uint32_t value = (raw & mask) >> shift;
        return bits >= 8 ? static_cast<uint8_t>(value >> (bits - 8))
                         : static_cast<uint8_t>(value << (8 - bits));
    }

    static bool copyTexture(const DDSURFACEDESC2 &desc, TextureCache &texture) {
        // DX7-era environment bump mapping (D3DFMT_V8U8 / D3DFMT_L6V5U5) uses
        // 16bpp surfaces with DDPF_BUMPDUDV[|BUMPLUMINANCE] instead of
        // DDPF_RGB. These used to be silently rejected here (dwRGBBitCount ==
        // 32 only), so a bump-mapped stage's texture never uploaded and
        // sampled as the shared white fallback instead - which is exactly
        // what turns bump-mapped water/lava into a flat plain colour.
        const bool isBump16 = desc.ddpfPixelFormat.dwRGBBitCount == 16 &&
                              (desc.ddpfPixelFormat.dwFlags & kDDPF_BUMPDUDV) != 0;
        bool valid = desc.lpSurface && desc.dwWidth && desc.dwHeight &&
                     desc.dwWidth <= 8192 && desc.dwHeight <= 8192 && desc.lPitch >= 0 &&
                     (desc.ddpfPixelFormat.dwRGBBitCount == 32 || isBump16);
        const uint64_t rowPitch = static_cast<uint64_t>(desc.dwWidth) * 4;
        const uint64_t dataSize = rowPitch * desc.dwHeight;
        if (dataSize > DK2M_SLOT_CAPACITY - sizeof(DK2MTextureUpdateCommand)) valid = false;
        if (!valid) return false;

        texture.width = desc.dwWidth;
        texture.height = desc.dwHeight;
        texture.rowPitch = static_cast<uint32_t>(rowPitch);
        texture.pixels.resize(static_cast<size_t>(dataSize));
        const auto *source = static_cast<const uint8_t *>(desc.lpSurface);

        if (isBump16) {
            const DWORD duMask = desc.ddpfPixelFormat.dwRBitMask;
            const DWORD dvMask = desc.ddpfPixelFormat.dwGBitMask;
            const bool hasLuminance = (desc.ddpfPixelFormat.dwFlags & kDDPF_BUMPLUMINANCE) != 0;
            const DWORD lumMask = desc.ddpfPixelFormat.dwBBitMask;
            // ponytail: one-shot per-process log of the actual masks seen, so a
            // wrong bit-layout guess (V8U8 vs L6V5U5, mask order) shows up as
            // data instead of having to be reverse-engineered from a screenshot.
            static bool loggedBumpFormat = false;
            if (!loggedBumpFormat) {
                loggedBumpFormat = true;
                gog_debugf("Metal bridge: bump surface %ux%u flags=0x%08x du_mask=0x%04x "
                           "dv_mask=0x%04x lum_mask=0x%04x has_luminance=%d",
                           desc.dwWidth, desc.dwHeight, desc.ddpfPixelFormat.dwFlags,
                           duMask, dvMask, lumMask, hasLuminance ? 1 : 0);
            }
            for (uint32_t y = 0; y < texture.height; ++y) {
                const auto *row = source + static_cast<size_t>(y) * desc.lPitch;
                uint8_t *destination = texture.pixels.data() + static_cast<size_t>(y) * texture.rowPitch;
                for (uint32_t x = 0; x < texture.width; ++x) {
                    uint16_t raw;
                    std::memcpy(&raw, row + x * 2, sizeof(raw));
                    uint8_t *pixel = destination + x * 4;
                    pixel[0] = signedBumpComponentToByte(raw, duMask);
                    pixel[1] = signedBumpComponentToByte(raw, dvMask);
                    pixel[2] = hasLuminance ? unsignedComponentToByte(raw, lumMask) : 128;
                    pixel[3] = 0xFF;
                }
            }
            return true;
        }

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
        cursor.displayWidth = width;
        cursor.displayHeight = height;
        cursor.mouseX = static_cast<int32_t>(x);
        cursor.mouseY = static_cast<int32_t>(y);
        cursor.visible = true;
        return true;
    }

    static bool captureNativeCursor(IDirectDrawSurface4 *source, DWORD width, DWORD height,
                                    DWORD colorKey, CursorSnapshot &cursor) {
        if (!width || !height) return false;

        RECT rect = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        DDSURFACEDESC2 lock = {};
        lock.dwSize = sizeof(lock);
        if (FAILED(source->Lock(&rect, &lock, DDLOCK_READONLY | DDLOCK_WAIT, nullptr)))
            return false;
        const bool source32 = lock.lpSurface && lock.lPitch >= 0 &&
                              lock.ddpfPixelFormat.dwRGBBitCount == 32 &&
                              lock.ddpfPixelFormat.dwBBitMask == 0x000000FF &&
                              lock.ddpfPixelFormat.dwGBitMask == 0x0000FF00 &&
                              lock.ddpfPixelFormat.dwRBitMask == 0x00FF0000;
        if (!source32) {
            source->Unlock(&rect);
            return false;
        }

        cursor.width = width;
        cursor.height = height;
        cursor.displayWidth = width;
        cursor.displayHeight = height;
        cursor.pixels.resize(static_cast<size_t>(width) * height * 4);
        const uint32_t rgbMask = lock.ddpfPixelFormat.dwRBitMask |
                                 lock.ddpfPixelFormat.dwGBitMask |
                                 lock.ddpfPixelFormat.dwBBitMask;
        const uint32_t keyLow = colorKey & rgbMask;
        const int keyR = (keyLow >> 16) & 0xFF, keyG = (keyLow >> 8) & 0xFF,
                  keyB = keyLow & 0xFF;
        for (uint32_t row = 0; row < height; ++row) {
            const auto *input = reinterpret_cast<const uint32_t *>(
                static_cast<const uint8_t *>(lock.lpSurface) +
                static_cast<size_t>(row) * lock.lPitch);
            auto *output = cursor.pixels.data() + static_cast<size_t>(row) * width * 4;
            for (uint32_t column = 0; column < width; ++column) {
                const uint32_t raw = input[column];
                const uint32_t rgb = raw & rgbMask;
                std::memcpy(output + column * 4, &raw, 3);
                if (rgb == keyLow) {
                    output[column * 4 + 3] = 0;
                    continue;
                }
                // The scaler bleeds the key colour into edge pixels; treat
                // near-key colours as transparent too (dark green halo fix).
                const int dr = ((rgb >> 16) & 0xFF) - keyR;
                const int dg = ((rgb >> 8) & 0xFF) - keyG;
                const int db = (rgb & 0xFF) - keyB;
                output[column * 4 + 3] =
                    (dr * dr + dg * dg + db * db) < 24 * 24 ? 0 : 0xFF;
            }
        }
        source->Unlock(&rect);
        // Coverage antialiasing for the keyed silhouette: alpha becomes the
        // 3x3 neighbourhood coverage of the binary mask, so the contour gets
        // fractional edge pixels instead of a hard staircase. Transparent
        // pixels that gain partial coverage inherit the average colour of
        // their opaque neighbours - without that the key colour would fringe
        // right back in through linear sampling.
        std::vector<uint8_t> &px = cursor.pixels;
        const size_t pixelCount = static_cast<size_t>(width) * height;
        std::vector<uint8_t> mask(pixelCount);
        for (size_t i = 0; i < pixelCount; ++i) mask[i] = px[i * 4 + 3] ? 1 : 0;
        std::vector<uint8_t> alphaOut(pixelCount);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const size_t idx = static_cast<size_t>(y) * width + x;
                uint32_t covered = 0, total = 0;
                uint32_t r = 0, g = 0, b = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int ny = static_cast<int>(y) + dy;
                    if (ny < 0 || ny >= static_cast<int>(height)) continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = static_cast<int>(x) + dx;
                        if (nx < 0 || nx >= static_cast<int>(width)) continue;
                        ++total;
                        const size_t nidx = static_cast<size_t>(ny) * width + nx;
                        if (!mask[nidx]) continue;
                        ++covered;
                        r += px[nidx * 4 + 2];
                        g += px[nidx * 4 + 1];
                        b += px[nidx * 4 + 0];
                    }
                }
                if (!covered) { alphaOut[idx] = 0; continue; }
                alphaOut[idx] = static_cast<uint8_t>(covered * 255u / total);
                if (!mask[idx]) {
                    px[idx * 4 + 2] = static_cast<uint8_t>(r / covered);
                    px[idx * 4 + 1] = static_cast<uint8_t>(g / covered);
                    px[idx * 4 + 0] = static_cast<uint8_t>(b / covered);
                }
            }
        }
        for (size_t i = 0; i < pixelCount; ++i) px[i * 4 + 3] = alphaOut[i];
        return true;
    }

    // No longer applied (see cursor()): kept as documentation of the 13
    // composed Hand-of-Evil sizes at the 640x480 base resolution.
    void normalizeCursorSize(CursorSnapshot &cursor) const {
        struct CursorSize { uint16_t width; uint16_t height; };
        static constexpr CursorSize originalSizes[] = {
            {82, 53}, {87, 104}, {64, 64}, {87, 95}, {88, 103},
            {126, 99}, {83, 39}, {88, 65}, {84, 86}, {63, 100},
            {63, 43}, {74, 70}, {42, 20},
        };
        if (width_ <= 640 || height_ <= 480) return;
        const float scaleX = static_cast<float>(width_) / 640.0f;
        const float scaleY = static_cast<float>(height_) / 480.0f;
        for (const CursorSize size : originalSizes) {
            const int expectedWidth = static_cast<int>(std::lround(size.width * scaleX));
            const int expectedHeight = static_cast<int>(std::lround(size.height * scaleY));
            if (std::abs(static_cast<int>(cursor.width) - expectedWidth) > 2 ||
                std::abs(static_cast<int>(cursor.height) - expectedHeight) > 2) continue;
            cursor.displayWidth = size.width;
            cursor.displayHeight = size.height;
            cursor.hotspotX = static_cast<int32_t>(std::lround(
                cursor.hotspotX * static_cast<float>(size.width) / cursor.width));
            cursor.hotspotY = static_cast<int32_t>(std::lround(
                cursor.hotspotY * static_cast<float>(size.height) / cursor.height));
            return;
        }
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

    // Marks every tile touched by `rect` as freshly written in generation
    // array `gen` (parityTileGen_[0] for cache[0]="black", [1] for "white").
    // updateOverlay() compares these against each OverlayTileState's last-
    // processed generation to decide which tiles actually need re-unmatting,
    // instead of inferring it from a single accumulated bounding rect that
    // can miss a tile whose two parity frames' dirty windows don't overlap.
    void markTilesTouched(std::vector<uint32_t> &gen, const RECT &rect) {
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
        for (uint32_t tileY = firstTileY; tileY <= lastTileY; ++tileY) {
            for (uint32_t tileX = firstTileX; tileX <= lastTileX; ++tileX) {
                uint32_t &g = gen[static_cast<size_t>(tileY) * overlayTileColumns_ + tileX];
                if (++g == 0) g = 1;  // skip 0, reserved for "never touched"
            }
        }
    }

    void updateOverlay(const TextureCache &black, const TextureCache &white,
                       const CursorSnapshot &) {
        if (overlay_.width != black.width || overlay_.height != black.height ||
            overlay_.rowPitch != black.rowPitch) {
            overlay_.width = black.width;
            overlay_.height = black.height;
            overlay_.rowPitch = black.rowPitch;
            overlay_.pixels.assign(static_cast<size_t>(black.rowPitch) * black.height, 0);
            overlayTileColumns_ = (black.width + kOverlayTileSize - 1) / kOverlayTileSize;
            overlayTileRows_ = (black.height + kOverlayTileSize - 1) / kOverlayTileSize;
            overlayTiles_.assign(static_cast<size_t>(overlayTileColumns_) * overlayTileRows_, {});
            parityTileGen_[0].assign(overlayTiles_.size(), 0);
            parityTileGen_[1].assign(overlayTiles_.size(), 0);
            overlayForceFull_ = true;
            // Both caches were just (re)captured in full at this new size:
            // touch every tile in both generation arrays once, so the loop
            // below finds every tile dirty against the freshly-reset (0)
            // lastBlackGen/lastWhiteGen on this very first pass.
            const RECT full = {0, 0, (LONG)black.width, (LONG)black.height};
            markTilesTouched(parityTileGen_[0], full);
            markTilesTouched(parityTileGen_[1], full);
        }

        // Per-tile generation compare replaces the old single accumulated
        // "process" rect: a tile is only skipped when NEITHER cache changed
        // it since we last unmatted it, so two parity frames whose dirty
        // windows don't overlap can no longer leave a tile stuck stale.
        for (uint32_t tileY = 0; tileY < overlayTileRows_; ++tileY) {
            const uint32_t yBegin = tileY * kOverlayTileSize;
            const uint32_t yEnd = std::min(yBegin + kOverlayTileSize, black.height);
            for (uint32_t tileX = 0; tileX < overlayTileColumns_; ++tileX) {
                const size_t tile = static_cast<size_t>(tileY) * overlayTileColumns_ + tileX;
                const uint32_t blackGen = parityTileGen_[0][tile];
                const uint32_t whiteGen = parityTileGen_[1][tile];
                OverlayTileState &state = overlayTiles_[tile];
                if (blackGen == state.lastBlackGen && whiteGen == state.lastWhiteGen) continue;

                const uint32_t xBegin = tileX * kOverlayTileSize;
                const uint32_t xEnd = std::min(xBegin + kOverlayTileSize, black.width);
                // Unmatte into a tile-sized scratch and only publish (and bump
                // the tile version, which is what schedules a bridge upload)
                // when the composited pixels actually changed. The game clears
                // and redraws the whole 2D layer every frame, so the input
                // mattes' generations tick on every tile every frame - but the
                // composited result is usually byte-identical, and version-on-
                // input-change degenerated into re-uploading the entire
                // 1600x1200 overlay (~7.7MB) through the bridge each frame.
                alignas(16) uint8_t scratch[kOverlayTileSize * kOverlayTileSize * 4];
                const uint32_t spanBytes = (xEnd - xBegin) * 4;
                for (uint32_t y = yBegin; y < yEnd; ++y) {
                    unmatteOverlaySpan(
                        black.pixels.data() + static_cast<size_t>(y) * black.rowPitch + static_cast<size_t>(xBegin) * 4,
                        white.pixels.data() + static_cast<size_t>(y) * white.rowPitch + static_cast<size_t>(xBegin) * 4,
                        scratch + static_cast<size_t>(y - yBegin) * spanBytes,
                        xEnd - xBegin);
                }
                bool changed = false;
                for (uint32_t y = yBegin; y < yEnd; ++y) {
                    uint8_t *dst = overlay_.pixels.data() +
                        static_cast<size_t>(y) * overlay_.rowPitch + static_cast<size_t>(xBegin) * 4;
                    const uint8_t *src = scratch + static_cast<size_t>(y - yBegin) * spanBytes;
                    if (!changed && std::memcmp(dst, src, spanBytes) != 0) changed = true;
                    if (changed) std::memcpy(dst, src, spanBytes);
                }
                state.lastBlackGen = blackGen;
                state.lastWhiteGen = whiteGen;
                if (changed) {
                    if (++state.version == 0) state.version = 1;
                }
            }
        }
    }

    bool captureTexture(IDirectDrawSurface4 *surface, TextureCache &texture) {
        if (!surface) return false;
        // SEH-guarded: level transitions destroy menu surfaces while the game
        // still passes their FakeTexture to SetTexture once more - Lock on the
        // freed surface page-faults (this was the deterministic level-load
        // crash once the stack finally resolved cleanly). A dead surface just
        // means "no capture", never a dead process.
        // negative cache first: re-faulting through Wine's WOW64 SEH dispatch
        // every frame provokes its known 7BF2123D instability - fault once
        // per dead surface, then refuse straight away.
        for (const void *dead : deadSurfaces_) {
            if (dead == surface) return false;
        }
        __try {
            DDSURFACEDESC2 desc = {};
            desc.dwSize = sizeof(desc);
            const HRESULT hr = surface->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_READONLY, nullptr);
            if (FAILED(hr)) return false;
            const bool valid = copyTexture(desc, texture);
            surface->Unlock(nullptr);
            return valid;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (deadSurfaces_.size() < 4096) deadSurfaces_.push_back(surface);
            static bool loggedDeadSurface = false;
            if (!loggedDeadSurface) {
                loggedDeadSurface = true;
                gog_debugf("Metal bridge: capture faulted on dead surface %p", surface);
            }
            return false;
        }
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

    void emitShadowMasks(DWORD textureId, IDirectDrawSurface4 *surface) {
        if (!textureId || !surface || !metalShadowsEnabled()) return;
        for (auto &entry : shadowMasks_) {
            ShadowMaskState &state = entry.second;
            if (!state.generation || state.lastSentGeneration == state.generation) continue;
            dk2::shadowgpu::TargetRegion target = {};
            if (!dk2::shadowgpu::resolveTarget(
                    reinterpret_cast<const void *>(entry.first), surface, &target)) {
                continue;
            }
            const uint64_t triangleBytes =
                static_cast<uint64_t>(state.triangles.size()) * sizeof(DK2MShadowTriangle);
            const uint64_t commandBytes = sizeof(DK2MShadowMaskCommand) + triangleBytes;
            if (commandBytes > UINT32_MAX || used_ + commandBytes > DK2M_SLOT_CAPACITY) {
                continue;
            }
            DK2MShadowMaskCommand command = {};
            command.header.type = DK2M_COMMAND_SHADOW_MASK;
            command.header.size = static_cast<uint32_t>(commandBytes);
            command.texture_id = textureId;
            command.target_x = target.x;
            command.target_y = target.y;
            command.target_width = target.width;
            command.target_height = target.height;
            command.triangle_count = static_cast<uint32_t>(state.triangles.size());
            command.mode = state.mode;
            append(&command, sizeof(command));
            if (triangleBytes) {
                append(state.triangles.data(), static_cast<uint32_t>(triangleBytes));
            }
            ++commandCount_;
            state.lastSentGeneration = state.generation;
        }
    }

    void emitTexture(DWORD stage, DWORD textureId,
                     IDirectDrawSurface4 *surface = nullptr) {
        bool textureReadyForShadow = false;
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
                textureReadyForShadow = !texture.pending || texture.sentInCurrentFrame;
            }
        }

        if (textureReadyForShadow) emitShadowMasks(textureId, surface);

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

    void emitCursor() {
        CursorSnapshot cursor;
        {
            std::lock_guard<std::mutex> lock(overlayMutex_);
            cursor = cursor_;
        }
        if (!cursor.visible || cursor.pixels.empty() ||
            !cursor.displayWidth || !cursor.displayHeight) return;

        constexpr WORD indices[] = {0, 1, 2, 0, 2, 3};
        const float left = static_cast<float>(cursor.mouseX - cursor.hotspotX);
        const float top = static_cast<float>(cursor.mouseY - cursor.hotspotY);
        const float right = left + static_cast<float>(cursor.displayWidth);
        const float bottom = top + static_cast<float>(cursor.displayHeight);
        const DK2MVertex1C vertices[] = {
            {left, top, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f},
            {right, top, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
            {right, bottom, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 1.0f},
            {left, bottom, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
        };
        const uint32_t dataSize = static_cast<uint32_t>(cursor.pixels.size());
        const uint32_t updateBytes = sizeof(DK2MTextureUpdateCommand) + dataSize;
        constexpr uint32_t stateBytes =
            sizeof(DK2MSetTextureCommand) + 6 * sizeof(DK2MRenderStateCommand) +
            6 * sizeof(DK2MTextureStageStateCommand);
        const uint32_t drawBytes = sizeof(DK2MDrawIndexedCommand) +
                                   sizeof(vertices) + sizeof(indices);
        if (used_ + updateBytes + stateBytes + drawBytes > DK2M_SLOT_CAPACITY) return;

        DK2MTextureUpdateCommand update = {};
        update.header.type = DK2M_COMMAND_TEXTURE_UPDATE;
        update.header.size = updateBytes;
        update.texture_id = DK2M_CURSOR_TEXTURE_ID;
        update.width = cursor.width;
        update.height = cursor.height;
        update.row_pitch = cursor.width * 4;
        update.data_size = dataSize;
        append(&update, sizeof(update));
        append(cursor.pixels.data(), dataSize);
        ++commandCount_;

        emitTexture(0, DK2M_CURSOR_TEXTURE_ID);
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

public:
    // --- world-space mesh pipeline (introduced in v9; retained/deformed v13) ---
    bool meshRegister(uint32_t meshId, const void *vertices, uint32_t vertexCount,
                      const uint16_t *indices, uint32_t indexCount) {
        if (!meshId || !vertices || !vertexCount || !indices || !indexCount ||
            !ensureMapped()) return false;
        MeshState &state = meshes_[meshId];
        if (!state.blob.empty()) return true;
        const uint32_t vertexBytes = vertexCount * static_cast<uint32_t>(sizeof(DK2MMeshVertex));
        const uint32_t indexBytes = (indexCount * static_cast<uint32_t>(sizeof(uint16_t)) + 3u) & ~3u;
        DK2MMeshRegisterCommand command = {};
        command.header.type = DK2M_COMMAND_MESH_REGISTER;
        command.header.size = static_cast<uint32_t>(sizeof(command)) + vertexBytes + indexBytes;
        command.mesh_id = meshId;
        command.vertex_count = vertexCount;
        command.index_count = indexCount;
        state.blob.resize(command.header.size);
        std::memcpy(state.blob.data(), &command, sizeof(command));
        std::memcpy(state.blob.data() + sizeof(command), vertices, vertexBytes);
        std::memset(state.blob.data() + sizeof(command) + vertexBytes, 0, indexBytes);
        std::memcpy(state.blob.data() + sizeof(command) + vertexBytes, indices,
                    indexCount * sizeof(uint16_t));
        return true;
    }

    void stageBytes(const void *data, uint32_t size) {
        const auto *bytes = static_cast<const uint8_t *>(data);
        stagedMesh_.insert(stagedMesh_.end(), bytes, bytes + size);
    }

    void cameraSet(const float viewProj[16], const float depthParams[6]) {
        DK2MCameraSetCommand command = {};
        command.header.type = DK2M_COMMAND_CAMERA_SET;
        command.header.size = sizeof(command);
        std::memcpy(command.view_proj, viewProj, sizeof(command.view_proj));
        command.z_mul2 = depthParams[0];
        command.z_add2 = depthParams[1];
        command.z_add3 = depthParams[2];
        command.z_mul3_f = depthParams[3];
        command.far_threshold = depthParams[4];
        command.depth_cap = depthParams[5];
        if (!active_) {
            if (stagedCamera_) return;
            stagedCamera_ = true;
            stageBytes(&command, sizeof(command));
            ++stagedMeshCommandCount_;
            return;
        }
        if (used_ + sizeof(command) > DK2M_SLOT_CAPACITY) return;
        append(&command, sizeof(command));
        ++commandCount_;
    }

    // Light lists arrive per mesh (each emitter call carries its spatially
    // selected subset), so callers send a growing per-frame union: keep only
    // the LAST payload and emit it once per frame - the host applies a single
    // lights buffer to every mesh draw of the frame anyway.
    void lightsSet(const void *lights, uint32_t lightCount, float ambientR,
                   float ambientG, float ambientB, const float falloffLut[256]) {
        const uint32_t lutBytes = 256u * sizeof(float);
        const uint32_t lightBytes = lightCount * static_cast<uint32_t>(sizeof(DK2MLight));
        const uint32_t size = static_cast<uint32_t>(sizeof(DK2MLightsSetCommand)) + lutBytes + lightBytes;
        DK2MLightsSetCommand command = {};
        command.header.type = DK2M_COMMAND_LIGHTS_SET;
        command.header.size = size;
        command.light_count = lightCount;
        command.ambient_r = ambientR;
        command.ambient_g = ambientG;
        command.ambient_b = ambientB;
        pendingLights_.clear();
        pendingLights_.reserve(size);
        const auto *cmdBytes = reinterpret_cast<const uint8_t *>(&command);
        pendingLights_.insert(pendingLights_.end(), cmdBytes, cmdBytes + sizeof(command));
        const auto *lutBytesPtr = reinterpret_cast<const uint8_t *>(falloffLut);
        pendingLights_.insert(pendingLights_.end(), lutBytesPtr, lutBytesPtr + lutBytes);
        if (lightBytes) {
            const auto *lightBytesPtr = static_cast<const uint8_t *>(lights);
            pendingLights_.insert(pendingLights_.end(), lightBytesPtr, lightBytesPtr + lightBytes);
        }
    }

    void flushPendingLights() {
        if (pendingLights_.empty() || !active_) return;
        if (used_ + pendingLights_.size() > DK2M_SLOT_CAPACITY) return;
        append(pendingLights_.data(), static_cast<uint32_t>(pendingLights_.size()));
        ++commandCount_;
        pendingLights_.clear();
    }

    void frameSize(uint32_t *width, uint32_t *height) const {
        if (width) *width = width_;
        if (height) *height = height_;
    }

    uint32_t frameCounter() const { return frame_; }

    void drawMeshInline(uint32_t textureId, const void *vertices, uint32_t vertexCount,
                        const uint16_t *indices, uint32_t indexCount, uint32_t tint,
                        uint32_t flags, const uint16_t *lightIndices,
                        uint32_t lightCount, float ambientR, float ambientG,
                        float ambientB) {
        if (!vertices || !vertexCount || !indices || !indexCount) return;
        lightCount = std::min<uint32_t>(lightCount, DK2M_MAX_LIGHTS_PER_DRAW);
        emitInlineCommand(textureId, vertices, vertexCount, indices, indexCount,
                          tint, flags, lightIndices, lightCount,
                          ambientR, ambientG, ambientB);
    }

    void emitInlineCommand(uint32_t textureId, const void *vertices, uint32_t vertexCount,
                           const uint16_t *indices, uint32_t indexCount, uint32_t tint,
                           uint32_t flags, const uint16_t *lightIndices,
                           uint32_t lightCount, float ambientR, float ambientG,
                           float ambientB) {
        if (!vertices || !vertexCount || !indices || !indexCount) return;
        const uint32_t vertexBytes = vertexCount * static_cast<uint32_t>(sizeof(DK2MMeshVertex));
        const uint32_t indexBytes = (indexCount * 2u + 3u) & ~3u;
        const uint32_t size = static_cast<uint32_t>(sizeof(DK2MDrawMeshInlineCommand)) +
                              vertexBytes + indexBytes;
        DK2MDrawMeshInlineCommand command = {};
        command.header.type = DK2M_COMMAND_DRAW_MESH_INLINE;
        command.header.size = size;
        command.texture_id = textureId;
        command.flags = flags;
        command.tint = tint;
        command.vertex_count = vertexCount;
        command.index_count = indexCount;
        command.ambient_r = ambientR;
        command.ambient_g = ambientG;
        command.ambient_b = ambientB;
        command.light_count = std::min<uint32_t>(
            lightCount, DK2M_MAX_LIGHTS_PER_DRAW);
        if (command.light_count) {
            std::memcpy(command.light_indices, lightIndices,
                        command.light_count * sizeof(uint16_t));
        }
        const uint16_t zeroPad = 0;
        const uint32_t padBytes = indexBytes - indexCount * 2u;
        if (!active_) {
            // Staged during prepare (sceneEmit runs between frames); flushed
            // into the next frame's head by begin().
            stageBytes(&command, sizeof(command));
            stageBytes(vertices, vertexBytes);
            stageBytes(indices, indexCount * 2u);
            if (padBytes) stageBytes(&zeroPad, padBytes);
            if (textureId) stagedMeshTextures_.push_back(textureId);
            ++stagedMeshCommandCount_;
            return;
        }
        if (used_ + size > DK2M_SLOT_CAPACITY) return;
        if (textureId) emitTexture(0, textureId);
        if (used_ + size > DK2M_SLOT_CAPACITY) return;
        append(&command, sizeof(command));
        append(vertices, vertexBytes);
        append(indices, indexCount * 2u);
        if (padBytes) append(&zeroPad, padBytes);
        ++commandCount_;
    }

    bool prepareMeshRegistration(uint32_t meshId) {
        auto found = meshes_.find(meshId);
        if (found == meshes_.end() || found->second.blob.empty() || !header_) return false;
        MeshState &state = found->second;
        // (Re)send the registration blob until the consumer acknowledges a
        // frame that carried it - same ack model as texture uploads, so a
        // consumer that attaches mid-session still receives every mesh.
        const uint32_t consumerSession =
            InterlockedCompareExchange(asLong(&header_->consumer_session), 0, 0);
        if (consumerSession != meshConsumerSession_) {
            meshConsumerSession_ = consumerSession;
            for (auto &entry : meshes_) {
                entry.second.pending = true;
                entry.second.lastSentFrame = 0;
            }
        }
        if (state.pending) {
            const uint32_t consumer =
                InterlockedCompareExchange(asLong(&header_->consumer_frame), 0, 0);
            if (state.lastSentFrame && consumer == state.lastSentFrame) {
                state.pending = false;
            } else if (state.lastSentFrame != frame_ + 1) {
                if (active_) {
                    if (used_ + state.blob.size() > DK2M_SLOT_CAPACITY) return false;
                    append(state.blob.data(), static_cast<uint32_t>(state.blob.size()));
                    ++commandCount_;
                } else {
                    stageBytes(state.blob.data(), static_cast<uint32_t>(state.blob.size()));
                    ++stagedMeshCommandCount_;
                }
                state.lastSentFrame = frame_ + 1;
                if (state.lastSentFrame == 0) state.lastSentFrame = 1;
            }
        }
        return true;
    }

    void emitRetainedCommand(const DK2MDrawMeshCommand &command, bool stage) {
        if (stage || !active_) {
            stageBytes(&command, sizeof(command));
            if (command.texture_id) {
                stagedMeshTextures_.push_back(command.texture_id);
            }
            ++stagedMeshCommandCount_;
            return;
        }
        if (command.texture_id) emitTexture(0, command.texture_id);
        if (used_ + sizeof(command) > DK2M_SLOT_CAPACITY) return;
        append(&command, sizeof(command));
        ++commandCount_;
    }

    void drawMesh(uint32_t meshId, uint32_t textureId, const float world[12],
                  const float uvTransform[4], uint32_t tint, uint32_t flags,
                  const uint16_t *lightIndices, uint32_t lightCount,
                  float ambientR, float ambientG, float ambientB) {
        if (!prepareMeshRegistration(meshId)) return;
        DK2MDrawMeshCommand command = {};
        command.header.type = DK2M_COMMAND_DRAW_MESH;
        command.header.size = sizeof(command);
        command.mesh_id = meshId;
        command.texture_id = textureId;
        command.flags = flags;
        command.tint = tint;
        command.ambient_r = ambientR;
        command.ambient_g = ambientG;
        command.ambient_b = ambientB;
        std::memcpy(command.world, world, sizeof(command.world));
        command.uv_scale_u = uvTransform[0];
        command.uv_scale_v = uvTransform[1];
        command.uv_offset_u = uvTransform[2];
        command.uv_offset_v = uvTransform[3];
        command.light_count = std::min<uint32_t>(
            lightCount, DK2M_MAX_LIGHTS_PER_DRAW);
        if (command.light_count) {
            std::memcpy(command.light_indices, lightIndices,
                        command.light_count * sizeof(uint16_t));
        }
        emitRetainedCommand(command, !active_);
    }

    void drawMeshDeformed(uint32_t meshId, uint32_t textureId,
                          const float *positions, uint32_t vertexCount,
                          const float world[12], const float uvTransform[4],
                          uint32_t tint, uint32_t flags,
                          const uint16_t *lightIndices, uint32_t lightCount,
                          float ambientR, float ambientG, float ambientB) {
        if (!positions || !vertexCount || !prepareMeshRegistration(meshId)) return;
        const uint32_t positionBytes = vertexCount * 3u * sizeof(float);
        const uint32_t size = sizeof(DK2MDrawMeshDeformedCommand) + positionBytes;
        DK2MDrawMeshDeformedCommand command = {};
        command.header.type = DK2M_COMMAND_DRAW_MESH_DEFORMED;
        command.header.size = size;
        command.mesh_id = meshId;
        command.texture_id = textureId;
        command.flags = flags;
        command.tint = tint;
        command.vertex_count = vertexCount;
        command.ambient_r = ambientR;
        command.ambient_g = ambientG;
        command.ambient_b = ambientB;
        std::memcpy(command.world, world, sizeof(command.world));
        command.uv_scale_u = uvTransform[0];
        command.uv_scale_v = uvTransform[1];
        command.uv_offset_u = uvTransform[2];
        command.uv_offset_v = uvTransform[3];
        command.light_count = std::min<uint32_t>(
            lightCount, DK2M_MAX_LIGHTS_PER_DRAW);
        if (command.light_count) {
            std::memcpy(command.light_indices, lightIndices,
                        command.light_count * sizeof(uint16_t));
        }
        if (!active_) {
            stageBytes(&command, sizeof(command));
            stageBytes(positions, positionBytes);
            if (textureId) stagedMeshTextures_.push_back(textureId);
            ++stagedMeshCommandCount_;
            return;
        }
        if (textureId) emitTexture(0, textureId);
        if (used_ + size > DK2M_SLOT_CAPACITY) return;
        append(&command, sizeof(command));
        append(positions, positionBytes);
        ++commandCount_;
    }

private:
    void forgetSurfaceKey(uintptr_t pageKey) {
        pendingAtlasRects_.erase(pageKey);
        const void *key = reinterpret_cast<const void *>(pageKey);
        deadSurfaces_.erase(
            std::remove(deadSurfaces_.begin(), deadSurfaces_.end(), key),
            deadSurfaces_.end());
        atlasSeenByPage_.erase(pageKey);
    }

    void append(const void *data, uint32_t size) {
        std::memcpy(static_cast<uint8_t *>(view_) + DK2M_SLOT_OFFSET(slotIndex_) + used_, data, size);
        used_ += size;
    }

    struct PendingAtlasRect {
        uint16_t x, y, w, h;
        char name[64];
    };
    std::vector<DK2MPageAtlasMapCommand> atlasRects_;
    size_t atlasRectsAcked_ = 0;
    size_t atlasRectsEmitted_ = 0;
    uint32_t atlasLastSentFrame_ = 0;
    uint32_t atlasReportCalls_ = 0;
    uint32_t atlasReported_ = 0;
    uint32_t atlasMipCanonicalized_ = 0;
    uint32_t atlasRejectedInvalid_ = 0;
    std::unordered_map<uintptr_t, std::vector<PendingAtlasRect>> pendingAtlasRects_;
    std::unordered_map<uintptr_t, std::unordered_set<uint64_t>> atlasSeenByPage_;
    std::vector<uint32_t> atlasResets_;
    size_t atlasResetsAcked_ = 0;
    size_t atlasResetsEmitted_ = 0;
    uint32_t atlasResetLastSentFrame_ = 0;
    uint32_t atlasResetCount_ = 0;
    std::vector<uint32_t> textureReleases_;
    size_t textureReleasesAcked_ = 0;
    size_t textureReleasesEmitted_ = 0;
    uint32_t textureReleaseLastSentFrame_ = 0;
    uint32_t textureReleaseCount_ = 0;

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
    IDirectDrawSurface4 *boundSurfaces_[3] = {};
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
    struct ShadowMaskState {
        std::vector<DK2MShadowTriangle> triangles;
        uint32_t mode = DK2M_SHADOW_MASK_ALPHA;
        uint32_t generation = 0;
        uint32_t lastSentGeneration = 0;
    };
    std::unordered_map<uintptr_t, ShadowMaskState> shadowMasks_;
    std::atomic<bool> metalShadowsEnabled_{false};
    struct MeshState {
        std::vector<uint8_t> blob;  // serialized DK2MMeshRegisterCommand + payload
        bool pending = true;
        uint32_t lastSentFrame = 0;
    };
    std::unordered_map<uint32_t, MeshState> meshes_;
    uint32_t meshConsumerSession_ = 0;
    // mesh commands issued between frames (game prepare phase), flushed by begin()
    std::vector<uint8_t> stagedMesh_;
    std::vector<uint32_t> stagedMeshTextures_;
    std::vector<uint8_t> pendingLights_;
    std::vector<const void *> deadSurfaces_;
    uint32_t nextSyntheticTextureId_ = 0x40000000u;
    uint32_t stagedMeshCommandCount_ = 0;
    bool stagedCamera_ = false;
    bool stagedLights_ = false;
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
    uint64_t lastCursorGeometry_ = 0;
    RECT parityDrawn_[2] = {};
    RECT drawnRect_ = {};
    bool drawnValid_ = false;
    bool drawnFull_ = false;
    bool clearedThisFrame_ = false;
    std::vector<OverlayTileState> overlayTiles_;
    // per-parity-cache, per-tile write generation; see markTilesTouched()
    std::vector<uint32_t> parityTileGen_[2];
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

bool metalShadowsEnabled() {
    return producer.metalShadowsEnabled();
}

bool headlessDirectDrawEnabled() {
    if (!isEnabled()) return false;
    char value[8] = {};
    const DWORD length = GetEnvironmentVariableA("DK2_HEADLESS_DDRAW", value, sizeof(value));
    return length == 0 || strcmp(value, "0") != 0;
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

void overlayBlt(IDirectDrawSurface4 *destination, const RECT *destinationRect,
                IDirectDrawSurface4 *source, const RECT *sourceRect,
                DWORD flags) {
    const DWORD x = destinationRect ? static_cast<DWORD>(std::max<LONG>(0, destinationRect->left)) : 0;
    const DWORD y = destinationRect ? static_cast<DWORD>(std::max<LONG>(0, destinationRect->top)) : 0;
    // MyDdSurfaceEx_BltWait uses bit 0 for source colour-keying and passes it
    // through to the DirectDraw wrapper.  Normalize it to BltFast's matching
    // flag so both cursor paths share the capture implementation.
    const DWORD captureFlags = (flags & 1) || (flags & DDBLT_KEYSRC)
                                   ? DDBLTFAST_SRCCOLORKEY : 0;
    producer.overlayBltFast(destination, x, y, source, sourceRect, captureFlags);
}

bool cursor(IDirectDrawSurface *source, DWORD width, DWORD height, DWORD colorKey,
            LONG mouseX, LONG mouseY, LONG hotspotX, LONG hotspotY) {
    return producer.cursor(source, width, height, colorKey,
                           mouseX, mouseY, hotspotX, hotspotY);
}

void hideCursor() { producer.hideCursor(); }

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

bool meshRegister(uint32_t meshId, const void *vertices, uint32_t vertexCount,
                  const uint16_t *indices, uint32_t indexCount) {
    return producer.meshRegister(meshId, vertices, vertexCount, indices, indexCount);
}

void cameraSet(const float viewProj[16], const float depthParams[6]) {
    producer.cameraSet(viewProj, depthParams);
}

void lightsSet(const void *lights, uint32_t lightCount, float ambientR, float ambientG,
               float ambientB, const float falloffLut[256]) {
    producer.lightsSet(lights, lightCount, ambientR, ambientG, ambientB, falloffLut);
}

void drawMesh(uint32_t meshId, uint32_t textureId, const float world[12],
              const float uvTransform[4], uint32_t tint, uint32_t flags,
              const uint16_t *lightIndices, uint32_t lightCount,
              float ambientR, float ambientG, float ambientB) {
    producer.drawMesh(meshId, textureId, world, uvTransform, tint, flags,
                      lightIndices, lightCount, ambientR, ambientG, ambientB);
}

void drawMeshDeformed(uint32_t meshId, uint32_t textureId,
                      const float *positions, uint32_t vertexCount,
                      const float world[12], const float uvTransform[4],
                      uint32_t tint, uint32_t flags,
                      const uint16_t *lightIndices, uint32_t lightCount,
                      float ambientR, float ambientG, float ambientB) {
    producer.drawMeshDeformed(
        meshId, textureId, positions, vertexCount, world, uvTransform,
        tint, flags,
        lightIndices, lightCount, ambientR, ambientG, ambientB);
}

void drawMeshInline(uint32_t textureId, const void *vertices, uint32_t vertexCount,
                    const uint16_t *indices, uint32_t indexCount, uint32_t tint,
                    uint32_t flags, const uint16_t *lightIndices,
                    uint32_t lightCount, float ambientR, float ambientG,
                    float ambientB) {
    producer.drawMeshInline(textureId, vertices, vertexCount, indices, indexCount,
                            tint, flags, lightIndices, lightCount,
                            ambientR, ambientG, ambientB);
}

void frameSize(uint32_t *width, uint32_t *height) { producer.frameSize(width, height); }

uint32_t frameCounter() { return producer.frameCounter(); }

void ensureTexture(DWORD textureId, IDirectDrawSurface4 *surface) {
    producer.ensureTexture(textureId, surface);
}

uint32_t ensureSurfaceTexture(IDirectDrawSurface4 *surface) {
    return producer.ensureSurfaceTexture(surface);
}

uint32_t ensureBufferTexture(const void *key, const void *pixels, uint32_t width,
                             uint32_t height, uint32_t pitchBytes) {
    return producer.ensureBufferTexture(key, pixels, width, height, pitchBytes);
}

bool bufferTextureNeedsRefresh(uint32_t textureId) {
    return producer.bufferTextureNeedsRefresh(textureId);
}

void surfaceReleased(const void *key) {
    if (isEnabled()) producer.surfaceReleased(key);
}

void textureReleased(DWORD textureId, const void *key) {
    if (isEnabled()) producer.textureReleased(textureId, key);
}

void atlasPageReset(const void *pageKey) {
    if (!isEnabled()) return;
    producer.atlasPageReset(pageKey);
}

void reportAtlasRect(const void *pageKey, const char *name, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h) {
    if (!isEnabled()) return;
    producer.reportAtlasRect(pageKey, name, x, y, w, h);
}

void shadowMask(const void *handleKey, const void *triangles,
                uint32_t triangleCount, uint32_t mode) {
    producer.shadowMask(handleKey, triangles, triangleCount, mode);
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
