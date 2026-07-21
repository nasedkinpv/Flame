#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#import <Metal/Metal.h>
#include <sys/file.h>
#include <fcntl.h>
#import <QuartzCore/QuartzCore.h>

#include "metal_bridge/DK2BridgeProtocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <sched.h>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#import <ImageIO/ImageIO.h>

// toml11 is header-only; this file is the only consumer of it on the macOS
// side (the Windows-side Flametal DLL has its own separate include of the
// same header under a __stdcall/thread_local workaround it needs for MSVC --
// not needed here on clang/arm64, so this is a plain include). The library's
// `operator"" _toml` user-defined literal (unused here) trips
// -Wdeprecated-literal-operator under this project's -Werror; silence it
// locally rather than patching the vendored header.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-literal-operator"
#include "toml.hpp"
#pragma clang diagnostic pop

@class DK2MetalView;

// --- settings.toml: the one user-facing config file (see .porting/settings-
// redesign-plan.md). OS-neutral sections/keys, hand-editable, parsed with
// toml11 (libs/Toml11-4.4.0/toml.hpp). Precedence: env DK2_* (a debug-only
// override layer, see macos/README.md) beats settings.toml on every key this
// namespace manages. [renderer] keys are live-applied (dispatch-source watch
// on the file); [game]/[patches]/[debug] compose the wine runner's
// environment at next launch only.
namespace dk2cfg {

struct Settings {
    // [game]
    int shadowLevel = 3;
    std::string resolution = "1600x1200";
    bool movies = false;
    std::string level;
    // [renderer]
    bool bloom = true;
    bool metalShadows = true;
    float renderScale = 1.0f;
    bool hdTextures = true;
    // [patches]
    bool meshGpuPath = false;
    bool shadowCache = true;
    bool debugProbes = false;
    // [debug]
    std::string winedebug = "-all";
};

inline uint32_t floatBits(float f) { uint32_t b; std::memcpy(&b, &f, sizeof(b)); return b; }
inline float bitsFloat(uint32_t b) { float f; std::memcpy(&f, &b, sizeof(f)); return f; }

// Live renderer knobs, read every frame; settings.toml is the base, an env
// var present at startup pins the value and the file watcher stops touching it.
std::atomic<bool> gBloomLive{true};
std::atomic<bool> gShadowsLive{true};
std::atomic<bool> gHdTexturesLive{true};
std::atomic<uint32_t> gRenderScaleBits{floatBits(1.0f)};

bool gEnvBloomSet = false;
bool gEnvShadowsSet = false;
bool gEnvScaleSet = false;

std::mutex gMutex;
Settings gSettings;           // current, guarded by gMutex -- Preferences window reads/writes this
Settings gLaunchSettings;     // snapshot taken at process start, for the restart-required dirty flag
std::atomic<bool> gRestartRequired{false};

// Set once the AppKit view exists (after DK2MetalView is defined later in
// this file); called after every reload so a render_scale change re-requests
// the drawable size without waiting for an unrelated window resize.
std::function<void()> gOnApplied;

NSString *supportDirectory() {
    NSString *dir = [NSHomeDirectory() stringByAppendingPathComponent:
                      @"Library/Application Support/Dungeon Keeper II"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES
                                              attributes:nil error:nil];
    return dir;
}

NSString *settingsPath() {
    return [supportDirectory() stringByAppendingPathComponent:@"settings.toml"];
}

NSString *prefixFlametalConfigPath() {
    return [supportDirectory() stringByAppendingPathComponent:
             @"prefix/drive_c/GOG Games/Dungeon Keeper 2/flametal/config.toml"];
}

// Migration only, from the prefix's flametal/config.toml [flametal]/[gog]
// sections -- simple line parsing is enough since this only ever reads the
// four booleans below and never runs again once settings.toml exists.
void migrateFromPrefixConfig(Settings &s) {
    std::ifstream in(prefixFlametalConfigPath().fileSystemRepresentation);
    if (!in) return;
    auto trim = [](std::string x) {
        const size_t a = x.find_first_not_of(" \t\r");
        if (a == std::string::npos) return std::string();
        const size_t b = x.find_last_not_of(" \t\r");
        return x.substr(a, b - a + 1);
    };
    std::string line, section;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        const bool boolVal = val == "true";
        if (section == "flametal") {
            if (key == "MetalShadows") s.metalShadows = boolVal;
            else if (key == "ShadowCache") s.shadowCache = boolVal;
            else if (key == "DebugProbes") s.debugProbes = boolVal;
        } else if (section == "gog") {
            if (key == "MeshGpuPath") s.meshGpuPath = boolVal;
        }
    }
    NSLog(@"DK2 settings: migrated MetalShadows/ShadowCache/DebugProbes/MeshGpuPath "
           "from the prefix's flametal/config.toml");
}

template <typename T>
T tomlGet(const toml::value &table, const std::string &key, T fallback) {
    if (!table.is_table()) return fallback;
    const auto &t = table.as_table();
    const auto it = t.find(key);
    if (it == t.end()) return fallback;
    try {
        return toml::get<T>(it->second);
    } catch (const std::exception &) {
        return fallback;
    }
}

const toml::value *tomlSection(const toml::value &root, const std::string &name) {
    if (!root.is_table()) return nullptr;
    const auto &t = root.as_table();
    const auto it = t.find(name);
    return it == t.end() ? nullptr : &it->second;
}

std::string tomlBool(bool b) { return b ? "true" : "false"; }

// TOML floats must contain a '.' or exponent -- "1" parses back as an
// integer, not a float, and toml::get<float> on an integer node throws.
std::string tomlFloat(float v) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(3) << v;
    std::string s = o.str();
    const size_t lastNonZero = s.find_last_not_of('0');
    s.erase(lastNonZero + 1, std::string::npos);
    if (!s.empty() && s.back() == '.') s += '0';
    return s;
}

// The full commented file, values interpolated -- regenerated whenever the
// Preferences window (or migration, on first run) saves, exactly like
// flametal_config.cpp's own "config is controlled by X" convention: manual
// edits to VALUES survive (they are re-read on next launch), manual edits to
// COMMENTS do not.
std::string renderSettingsFile(const Settings &s) {
    std::ostringstream o;
    o << "# Dungeon Keeper II settings.\n"
      << "# The single user-facing config file: hand-edit this, or use the native\n"
      << "# Preferences window (Cmd-,) in the game host. OS-neutral format/sections.\n"
      << "# Regenerated on every Preferences save -- manual VALUE edits are kept,\n"
      << "# manual COMMENT edits are not (same convention as flametal/config.toml).\n"
      << "#\n"
      << "# Env vars named DK2_* are an undocumented-to-players, debug-only override\n"
      << "# layer on top of this file (see macos/README.md) -- if one is set in the\n"
      << "# environment at launch, it wins over the matching key below.\n\n";
    o << "[game]\n"
      << "# Dynamic shadow detail, 0 (off) - 3 (full detail, cached silhouettes)\n"
      << "shadow_level = " << s.shadowLevel << "\n"
      << "# \"WIDTHxHEIGHT\". Default 1600x1200 is 4:3. Wider values (e.g. 1800x1200)\n"
      << "# are experimental: the in-game HUD lays out panel backgrounds for 4:3, so\n"
      << "# wider resolutions show black gaps beside the bottom toolbar.\n"
      << "resolution = \"" << s.resolution << "\"\n"
      << "movies = " << tomlBool(s.movies) << "\n"
      << "# Level file name to load directly, skipping the normal menu. Empty = normal menu.\n"
      << "level = \"" << s.level << "\"\n\n";
    o << "[renderer]\n"
      << "# All four keys in this section apply live (no restart) while the game is running.\n"
      << "bloom = " << tomlBool(s.bloom) << "\n"
      << "metal_shadows = " << tomlBool(s.metalShadows) << "\n"
      << "# Fraction of the display's native backing resolution to render at, 0.375-1.0\n"
      << "render_scale = " << tomlFloat(s.renderScale) << "\n"
      << "hd_textures = " << tomlBool(s.hdTextures) << "\n\n";
    o << "[patches]\n"
      << "mesh_gpu_path = " << tomlBool(s.meshGpuPath) << "\n"
      << "shadow_cache = " << tomlBool(s.shadowCache) << "\n"
      << "debug_probes = " << tomlBool(s.debugProbes) << "\n\n";
    o << "[debug]\n"
      << "winedebug = \"" << s.winedebug << "\"\n";
    return o.str();
}

bool writeAtomic(const Settings &s) {
    NSString *path = settingsPath();
    NSString *tmp = [path stringByAppendingString:@".tmp"];
    const std::string text = renderSettingsFile(s);
    {
        std::ofstream out(tmp.fileSystemRepresentation, std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out) return false;
    }
    if (std::rename(tmp.fileSystemRepresentation, path.fileSystemRepresentation) != 0) {
        NSLog(@"DK2 settings: failed to replace %@ (%s)", path, std::strerror(errno));
        return false;
    }
    return true;
}

Settings load() {
    Settings s;
    NSString *path = settingsPath();
    if (![NSFileManager.defaultManager fileExistsAtPath:path]) {
        migrateFromPrefixConfig(s);
        if (!writeAtomic(s)) {
            NSLog(@"DK2 settings: failed to create %@; continuing with defaults in memory", path);
        } else {
            NSLog(@"DK2 settings: created %@", path);
        }
        return s;
    }
    try {
        const toml::value data = toml::parse(std::string(path.fileSystemRepresentation));
        if (const auto *g = tomlSection(data, "game")) {
            s.shadowLevel = tomlGet<int>(*g, "shadow_level", s.shadowLevel);
            s.resolution = tomlGet<std::string>(*g, "resolution", s.resolution);
            s.movies = tomlGet<bool>(*g, "movies", s.movies);
            s.level = tomlGet<std::string>(*g, "level", s.level);
        }
        if (const auto *r = tomlSection(data, "renderer")) {
            s.bloom = tomlGet<bool>(*r, "bloom", s.bloom);
            s.metalShadows = tomlGet<bool>(*r, "metal_shadows", s.metalShadows);
            s.renderScale = tomlGet<float>(*r, "render_scale", s.renderScale);
            s.hdTextures = tomlGet<bool>(*r, "hd_textures", s.hdTextures);
        }
        if (const auto *p = tomlSection(data, "patches")) {
            s.meshGpuPath = tomlGet<bool>(*p, "mesh_gpu_path", s.meshGpuPath);
            s.shadowCache = tomlGet<bool>(*p, "shadow_cache", s.shadowCache);
            s.debugProbes = tomlGet<bool>(*p, "debug_probes", s.debugProbes);
        }
        if (const auto *d = tomlSection(data, "debug")) {
            s.winedebug = tomlGet<std::string>(*d, "winedebug", s.winedebug);
        }
    } catch (const std::exception &e) {
        NSLog(@"DK2 settings.toml: parse error (%s); using defaults", e.what());
    }
    s.shadowLevel = std::clamp(s.shadowLevel, 0, 3);
    s.renderScale = std::clamp(s.renderScale, 0.375f, 1.0f);
    return s;
}

void initEnvOverrides() {
    gEnvBloomSet = std::getenv("DK2_BLOOM") != nullptr;
    gEnvShadowsSet = std::getenv("DK2_METAL_SHADOWS") != nullptr;
    gEnvScaleSet = std::getenv("DK2_RENDER_SCALE") != nullptr;
}

// Updates only the live-appliable atomics (and the dirty/restart flag); does
// not touch the environment -- see composeRunnerEnv for that. hd_textures has
// no env override (env-independent gating, per the settings plan) -- the
// unrelated DK2_TEXTURE_HD env var only ever selects the HD directory path.
void applyRendererLive(const Settings &s) {
    // One-time transition logs so a live toggle is verifiable in the log
    // without guessing whether the frame-time check ever saw the change.
    if (!gEnvBloomSet) {
        const bool was = gBloomLive.exchange(s.bloom, std::memory_order_relaxed);
        if (was != s.bloom) NSLog(@"DK2 settings: bloom -> %s", s.bloom ? "on" : "off");
    }
    if (!gEnvShadowsSet) {
        const bool was = gShadowsLive.exchange(s.metalShadows, std::memory_order_relaxed);
        if (was != s.metalShadows) NSLog(@"DK2 settings: metal_shadows -> %s", s.metalShadows ? "on" : "off");
    }
    {
        const bool was = gHdTexturesLive.exchange(s.hdTextures, std::memory_order_relaxed);
        if (was != s.hdTextures) NSLog(@"DK2 settings: hd_textures -> %s", s.hdTextures ? "on" : "off");
    }
    if (!gEnvScaleSet) {
        const float clamped = std::clamp(s.renderScale, 0.375f, 1.0f);
        const uint32_t was = gRenderScaleBits.exchange(floatBits(clamped), std::memory_order_relaxed);
        if (was != floatBits(clamped)) {
            NSLog(@"DK2 settings: render_scale -> %.3f", (double)clamped);
        }
    }
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gSettings = s;
        gRestartRequired.store(
            s.shadowLevel != gLaunchSettings.shadowLevel ||
            s.resolution != gLaunchSettings.resolution ||
            s.movies != gLaunchSettings.movies ||
            s.level != gLaunchSettings.level ||
            s.meshGpuPath != gLaunchSettings.meshGpuPath ||
            s.shadowCache != gLaunchSettings.shadowCache ||
            s.debugProbes != gLaunchSettings.debugProbes ||
            s.winedebug != gLaunchSettings.winedebug,
            std::memory_order_relaxed);
    }
    if (gOnApplied) gOnApplied();
}

void setenvIfUnset(const char *name, const std::string &value) {
    if (!std::getenv(name)) setenv(name, value.c_str(), 1);
}

// Composes the wine runner's environment from GAME/PATCHES/DEBUG settings.
// Only called once, at startup, before the runner process is spawned --
// game/patches/debug changes made later apply on next launch, per the
// restart-required dirty flag. setenvIfUnset means anything already in the
// environment (dev scripts' --runner-env, or a plain exported shell var)
// keeps winning over settings.toml, matching the env-overrides-file rule --
// EXCEPT DK2_EXTRA_GAME_ARGS (see below).
void composeRunnerEnv(const Settings &s) {
    setenvIfUnset("DK2_SHADOW_LEVEL", std::to_string(s.shadowLevel));
    setenvIfUnset("DK2_GAME_RES", s.resolution);
    setenvIfUnset("DK2_LEVEL", s.level);
    setenvIfUnset("DK2_WINEDEBUG", s.winedebug);
    setenvIfUnset("DK2_MOVIES", s.movies ? "1" : "0");
    std::ostringstream extra;
    extra << "-gog:MeshGpuPath=" << tomlBool(s.meshGpuPath)
          << " -flametal:ShadowCache=" << tomlBool(s.shadowCache)
          << " -flametal:DebugProbes=" << tomlBool(s.debugProbes);
    // Unlike the vars above, DK2_EXTRA_GAME_ARGS is not itself a documented
    // debug override -- it is this function's own output, an internal pipe
    // from host process to runner script. setenvIfUnset here would let a
    // value exported in a earlier/unrelated shell session (or left over from
    // a previous run in the same terminal) silently outlive the settings it
    // was built from after the source settings changed. Always overwrite.
    setenv("DK2_EXTRA_GAME_ARGS", extra.str().c_str(), 1);
    NSLog(@"DK2 settings: DK2_EXTRA_GAME_ARGS = %s", extra.str().c_str());
}

// Dispatch-source file watcher (the plan's "FSEvents or dispatch source on
// the file" -- a vnode source needs no extra framework and no run-loop
// plumbing beyond the main queue the app already pumps). Re-opens on
// delete/rename because the Preferences window's writer replaces the file
// via a temp-file rename rather than editing it in place.
dispatch_source_t gWatchSource;

void armWatcher() {
    if (gWatchSource) {
        dispatch_source_cancel(gWatchSource);
        gWatchSource = nil;
    }
    const int fd = open(settingsPath().fileSystemRepresentation, O_EVTONLY);
    if (fd < 0) return;
    dispatch_source_t source = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_VNODE, (uintptr_t)fd,
            DISPATCH_VNODE_WRITE | DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME,
            dispatch_get_main_queue());
    if (!source) { close(fd); return; }
    dispatch_source_set_cancel_handler(source, ^{ close(fd); });
    dispatch_source_set_event_handler(source, ^{
        const unsigned long flags = dispatch_source_get_data(source);
        if (flags & (DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME)) {
            // Preferences saves replace the file via temp-file rename, which
            // orphans this fd's vnode -- close it and watch the new inode.
            armWatcher();
            return;
        }
        applyRendererLive(load());
    });
    gWatchSource = source;
    dispatch_resume(source);
}

void startWatching() { armWatcher(); }

}  // namespace dk2cfg

// Texture dump mode: set DK2_TEXTURE_DUMP=1 (or =/abs/path) to write every
// unique texture crossing the bridge as PNG, keyed by a content hash. The
// same hash will later key HD replacements, so filenames are stable across
// runs. Animated surfaces produce one file per unique frame.
namespace texdump {

uint64_t contentHash(const uint8_t *row, uint32_t width, uint32_t height, uint32_t pitch) {
    uint64_t hash = 1469598103934665603ULL;  // FNV-1a 64
    for (uint32_t y = 0; y < height; ++y, row += pitch) {
        for (uint32_t x = 0; x < width * 4; ++x) {
            hash ^= row[x];
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

NSString *directory() {
    static NSString *dir = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *env = std::getenv("DK2_TEXTURE_DUMP");
        if (!env || !*env) return;
        NSString *path = env[0] == '/'
                ? @(env)
                : [NSHomeDirectory() stringByAppendingPathComponent:
                          @"Library/Application Support/Dungeon Keeper II/texture-dump"];
        if ([[NSFileManager defaultManager] createDirectoryAtPath:path
                                      withIntermediateDirectories:YES
                                                       attributes:nil
                                                            error:nil]) {
            dir = path;
        }
    });
    return dir;
}

dispatch_queue_t writeQueue() {
    static dispatch_queue_t queue =
            dispatch_queue_create("dk2.texture-dump", DISPATCH_QUEUE_SERIAL);
    return queue;
}

// A texture id that keeps producing new unique frames is a dynamically
// rendered surface (minimap, status panels), not an asset - stop dumping it
// and move what it already produced into dynamic/. Cyclic animations
// (torches) revisit the same hashes, so they stay below the limit.
constexpr size_t kDynamicFramesLimit = 40;

struct IdState {
    std::unordered_set<uint64_t> uniqueHashes;
    std::vector<NSString *> files;
    std::vector<uint8_t> prev;  // tight-packed BGRA of the last update
    uint32_t prevWidth = 0, prevHeight = 0;
    bool dynamic = false;
};

void writePng(NSString *file, NSMutableData *copy, uint32_t width, uint32_t height,
              uint64_t hash, uint32_t textureId, const char *kind) {
    // X8R8G8B8 sources carry zero alpha everywhere - make those opaque so the
    // dump is viewable; textures that really use alpha are left untouched
    auto *out = static_cast<uint8_t *>(copy.mutableBytes);
    bool anyAlpha = false;
    for (size_t i = 3; i < copy.length; i += 4) {
        if (out[i]) { anyAlpha = true; break; }
    }
    if (!anyAlpha) {
        for (size_t i = 3; i < copy.length; i += 4) out[i] = 0xFF;
    }
    std::string kindCopy = kind;
    dispatch_async(writeQueue(), ^{
        NSString *index = [directory() stringByAppendingPathComponent:@"index.csv"];
        FILE *f = fopen(index.fileSystemRepresentation, "a");
        if (f) {
            fprintf(f, "%016llx,%u,%ux%u,%s\n", hash, textureId, width, height,
                    kindCopy.c_str());
            fclose(f);
        }
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGDataProviderRef provider =
                CGDataProviderCreateWithCFData((__bridge CFDataRef)copy);
        CGImageRef image = CGImageCreate(
                width, height, 8, 32, width * 4, space,
                (CGBitmapInfo)kCGImageAlphaFirst | kCGBitmapByteOrder32Little,
                provider, NULL, false, kCGRenderingIntentDefault);
        if (image) {
            CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
                    (__bridge CFURLRef)[NSURL fileURLWithPath:file],
                    CFSTR("public.png"), 1, NULL);
            if (destination) {
                CGImageDestinationAddImage(destination, image, NULL);
                CGImageDestinationFinalize(destination);
                CFRelease(destination);
            }
            CGImageRelease(image);
        }
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(space);
    });
}

// Collage classifier (same heuristics as tools/curate_textures.py): a page
// with a few 32x32 blocks whose mean colour is alien to an otherwise
// homogeneous page, or with sprite colorkey pixels, is a cache-page collage
// and goes to collage/ instead of polluting the upscale set.
bool looksLikeCollage(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t pitch) {
    if (width < 64 || height < 64) return false;
    int colorkey = 0;
    float means[64][3];
    const uint32_t cols = width / 32, rows = height / 32;
    if (cols * rows > 64) return false;
    for (uint32_t by = 0; by < rows; ++by) {
        for (uint32_t bx = 0; bx < cols; ++bx) {
            uint32_t r = 0, g = 0, b = 0;
            for (uint32_t y = by * 32; y < by * 32 + 32; ++y) {
                const uint8_t *px = pixels + (size_t)y * pitch + (size_t)bx * 32 * 4;
                for (uint32_t x = 0; x < 32; ++x, px += 4) {
                    b += px[0]; g += px[1]; r += px[2];
                    if ((px[2] < 60 && px[1] > 200 && px[0] > 200) ||
                        (px[2] > 200 && px[1] < 60 && px[0] > 200) ||
                        (px[2] < 60 && px[1] > 220 && px[0] < 60))
                        ++colorkey;
                }
            }
            float *m = means[by * cols + bx];
            m[0] = r / 1024.0f; m[1] = g / 1024.0f; m[2] = b / 1024.0f;
        }
    }
    if (colorkey > 8) return true;
    // fire baked on a black tile (torch/heart pages): a 32x32 block that is
    // both mostly black and carries a cluster of warm pixels. Four different
    // tiles defeat the alien-block test and fire on black is not colorkey, so
    // this needs its own signal.
    for (uint32_t by = 0; by + 32 <= height; by += 32) {
        for (uint32_t bx = 0; bx + 32 <= width; bx += 32) {
            uint32_t fire = 0, black = 0;
            for (uint32_t y = by; y < by + 32; ++y) {
                const uint8_t *px = pixels + (size_t)y * pitch + (size_t)bx * 4;
                for (uint32_t x = 0; x < 32; ++x, px += 4) {
                    const int b = px[0], g = px[1], r = px[2];
                    if (r > 150 && g > 60 && b < 110 && r > b + 80) ++fire;
                    if (r + g + b < 75) ++black;
                }
            }
            if (fire > 25 && black > 150) return true;
        }
    }
    const uint32_t n = cols * rows;
    float med[3];
    for (int c = 0; c < 3; ++c) {
        float v[64];
        for (uint32_t i = 0; i < n; ++i) v[i] = means[i][c];
        std::nth_element(v, v + n / 2, v + n);
        med[c] = v[n / 2];
    }
    float devs[64];
    for (uint32_t i = 0; i < n; ++i) {
        devs[i] = (fabsf(means[i][0] - med[0]) + fabsf(means[i][1] - med[1]) +
                   fabsf(means[i][2] - med[2])) / 3.0f;
    }
    uint32_t outliers = 0, restCount = 0;
    float rest[64];
    for (uint32_t i = 0; i < n; ++i) {
        if (devs[i] > 28.0f) ++outliers;
        else rest[restCount++] = devs[i];
    }
    if (outliers < 1 || outliers > 6 || restCount == 0) return false;
    std::nth_element(rest, rest + restCount / 2, rest + restCount);
    return rest[restCount / 2] < 10.0f;
}

void emitSprite(NSString *dir, const uint8_t *pixels, uint32_t pitch, uint32_t textureId,
                uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    const uint32_t sw = x1 - x0, sh = y1 - y0;
    if (!sw || !sh) return;
    NSMutableData *copy = [NSMutableData dataWithLength:(NSUInteger)sw * sh * 4];
    auto *out = static_cast<uint8_t *>(copy.mutableBytes);
    for (uint32_t y = 0; y < sh; ++y) {
        std::memcpy(out + (size_t)y * sw * 4,
                    pixels + (size_t)(y0 + y) * pitch + x0 * 4, (size_t)sw * 4);
    }
    const uint64_t hash = contentHash(out, sw, sh, sw * 4);
    static std::unordered_set<uint64_t> seenSprites;
    if (!seenSprites.insert(hash).second) return;
    NSString *spriteDir = [dir stringByAppendingPathComponent:@"sprites"];
    [[NSFileManager defaultManager] createDirectoryAtPath:spriteDir
                              withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *file = [spriteDir stringByAppendingPathComponent:
            [NSString stringWithFormat:@"%016llx_%ux%u_at_%u_%u.png",
                                       hash, sw, sh, x0, y0]];
    if ([[NSFileManager defaultManager] fileExistsAtPath:file]) return;
    writePng(file, copy, sw, sh, hash, textureId, "sprite");
}

// render thread only
void dump(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t pitch,
          uint32_t textureId) {
    NSString *dir = directory();
    if (!dir) return;
    static std::unordered_map<uint32_t, IdState> perId;
    IdState &state = perId[textureId];

    // diff against the previous content of this id: partial updates are
    // sprite frames baked into a shared cache page. Changed rows are split
    // into bands separated by clean gaps, so a tile blit and a sprite blit
    // landing in one update become separate assets instead of one collage.
    bool partial = false;
    struct Band { uint32_t x0, y0, x1, y1; };
    Band bands[4];
    uint32_t bandCount = 0;
    const bool havePrev = state.prevWidth == width && state.prevHeight == height &&
                          !state.prev.empty();
    if (havePrev) {
        std::vector<uint32_t> rowX0(height), rowX1(height);
        bool any = false;
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t *n = pixels + (size_t)y * pitch;
            const uint8_t *p = state.prev.data() + (size_t)y * width * 4;
            rowX0[y] = 1; rowX1[y] = 0;
            if (!std::memcmp(n, p, (size_t)width * 4)) continue;
            uint32_t x = 0, e = width;
            while (x < width && !std::memcmp(n + x * 4, p + x * 4, 4)) ++x;
            while (e > x && !std::memcmp(n + (e - 1) * 4, p + (e - 1) * 4, 4)) --e;
            rowX0[y] = x; rowX1[y] = e;
            any = true;
        }
        if (!any) return;  // no change at all
        uint64_t changedArea = 0;
        uint32_t y = 0;
        bool overflow = false;
        while (y < height) {
            if (rowX0[y] >= rowX1[y]) { ++y; continue; }
            Band band{rowX0[y], y, rowX1[y], y + 1};
            uint32_t gap = 0;
            for (uint32_t r = y + 1; r < height && gap < 8; ++r) {
                if (rowX0[r] >= rowX1[r]) { ++gap; continue; }
                gap = 0;
                band.x0 = std::min(band.x0, rowX0[r]);
                band.x1 = std::max(band.x1, rowX1[r]);
                band.y1 = r + 1;
            }
            changedArea += (uint64_t)(band.x1 - band.x0) * (band.y1 - band.y0);
            if (bandCount < 4) bands[bandCount++] = band;
            else overflow = true;
            y = band.y1 + 1;
        }
        partial = !overflow && changedArea * 2 < (uint64_t)width * height;
    }
    // remember current content (tight copy)
    state.prev.resize((size_t)width * height * 4);
    state.prevWidth = width; state.prevHeight = height;
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(state.prev.data() + (size_t)y * width * 4,
                    pixels + (size_t)y * pitch, (size_t)width * 4);
    }

    if (partial) {
        // this id just proved it hosts animated sub-regions: any full-page
        // snapshot we wrote earlier has stale sprite frames baked in - move
        // those to collage/ retroactively
        if (!state.files.empty()) {
            NSString *collageDir = [dir stringByAppendingPathComponent:@"collage"];
            std::vector<NSString *> files = std::move(state.files);
            dispatch_async(writeQueue(), ^{
                NSFileManager *fm = NSFileManager.defaultManager;
                [fm createDirectoryAtPath:collageDir withIntermediateDirectories:YES
                               attributes:nil error:nil];
                for (NSString *path : files) {
                    [fm moveItemAtPath:path
                                toPath:[collageDir stringByAppendingPathComponent:
                                               path.lastPathComponent]
                                 error:nil];
                }
            });
        }
        for (uint32_t i = 0; i < bandCount; ++i) {
            emitSprite(dir, pixels, pitch, textureId,
                       bands[i].x0, bands[i].y0, bands[i].x1, bands[i].y1);
        }
        return;
    }

    if (state.dynamic) return;
    const uint64_t hash = contentHash(pixels, width, height, pitch);
    if (!state.uniqueHashes.insert(hash).second) return;
    const bool collage = looksLikeCollage(pixels, width, height, pitch);
    NSString *pageDir = dir;
    if (collage) {
        pageDir = [dir stringByAppendingPathComponent:@"collage"];
        [[NSFileManager defaultManager] createDirectoryAtPath:pageDir
                                  withIntermediateDirectories:YES attributes:nil error:nil];
    }
    NSString *file = [pageDir stringByAppendingPathComponent:
            [NSString stringWithFormat:@"%016llx_%ux%u.png", hash, width, height]];
    state.files.push_back(file);

    if (state.uniqueHashes.size() > kDynamicFramesLimit) {
        state.dynamic = true;
        NSString *dynamicDir = [dir stringByAppendingPathComponent:@"dynamic"];
        std::vector<NSString *> files = std::move(state.files);
        dispatch_async(writeQueue(), ^{
            NSFileManager *fm = NSFileManager.defaultManager;
            [fm createDirectoryAtPath:dynamicDir withIntermediateDirectories:YES
                           attributes:nil error:nil];
            for (NSString *path : files) {
                [fm moveItemAtPath:path
                            toPath:[dynamicDir stringByAppendingPathComponent:
                                           path.lastPathComponent]
                             error:nil];
            }
        });
        NSLog(@"texdump: texture id %u flagged dynamic, %lu frames moved",
              textureId, (unsigned long)files.size());
        return;
    }

    static std::unordered_set<uint64_t> seen;
    if (!seen.insert(hash).second) return;
    if ([[NSFileManager defaultManager] fileExistsAtPath:file]) return;

    NSMutableData *copy = [NSMutableData dataWithLength:(NSUInteger)width * height * 4];
    auto *out = static_cast<uint8_t *>(copy.mutableBytes);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(out + (size_t)y * width * 4, pixels + (size_t)y * pitch, (size_t)width * 4);
    }
    writePng(file, copy, width, height, hash, textureId, collage ? "collage" : "page");
}

}  // namespace texdump

// HD texture replacement: when <hash>.png exists in the HD directory
// (DK2_TEXTURE_HD=/abs/path, or the default below), it is loaded once, given
// a full CPU-built mip chain, and bound instead of the bridge texture. A
// missing or deleted file silently falls back to the original pixels.
namespace texhd {

// The candidate path (env override or the default under Application Support)
// never changes at runtime, so dispatch_once for that part is fine; but
// whether that path currently EXISTS is re-checked on every call rather than
// cached once. It used to be dispatch_once'd together with the existence
// check, which meant: (a) toggling hd_textures off then on again in the
// Preferences window couldn't be undone if the directory hadn't existed at
// the very first lookup (this cached a permanent nil, live flag or not), and
// (b) a directory created after the game started (e.g. the user drops HD
// textures in mid-session) was never picked up. Re-stat'ing here only costs
// anything on the relatively rare path where a texture update triggers a
// lookup, not per frame.
NSString *candidatePath() {
    static NSString *path = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *env = std::getenv("DK2_TEXTURE_HD");
        path = (env && *env)
                ? @(env)
                : [NSHomeDirectory() stringByAppendingPathComponent:
                          @"Library/Application Support/Dungeon Keeper II/textures-hd"];
    });
    return path;
}

NSString *directory() {
    // settings.toml [renderer] hd_textures, live-appliable, env-independent.
    // Toggling this off does not retroactively evict already-loaded HD
    // textures (see the LRU eviction in lookup() below) -- only newly
    // requested/reloaded pages are affected, converging as the LRU turns over.
    if (!dk2cfg::gHdTexturesLive.load(std::memory_order_relaxed)) return nil;
    NSString *path = candidatePath();
    BOOL isDir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&isDir] || !isDir) {
        return nil;
    }
    return path;
}

// CPU box-filter mip chain for an already-created mipmapped texture whose
// level 0 was just replaced. Rect updates leave upper mips stale; static
// page content changes rarely enough that the next full update refreshes them.
void fillMipChain(id<MTLTexture> texture, const uint8_t *pixels,
                  uint32_t width, uint32_t height, uint32_t rowPitch) {
    if (texture.mipmapLevelCount <= 1) return;
    std::vector<uint8_t> level((size_t)width * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(level.data() + (size_t)y * width * 4,
                    pixels + (size_t)y * rowPitch, (size_t)width * 4);
    }
    uint32_t w = width, h = height;
    for (NSUInteger mip = 1; mip < texture.mipmapLevelCount; ++mip) {
        const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        std::vector<uint8_t> next((size_t)nw * nh * 4);
        for (uint32_t y = 0; y < nh; ++y) {
            const uint32_t sy0 = std::min(y * 2, h - 1), sy1 = std::min(y * 2 + 1, h - 1);
            for (uint32_t x = 0; x < nw; ++x) {
                const uint32_t sx0 = std::min(x * 2, w - 1), sx1 = std::min(x * 2 + 1, w - 1);
                for (int c = 0; c < 4; ++c) {
                    const unsigned sum = level[((size_t)sy0 * w + sx0) * 4 + c]
                                       + level[((size_t)sy0 * w + sx1) * 4 + c]
                                       + level[((size_t)sy1 * w + sx0) * 4 + c]
                                       + level[((size_t)sy1 * w + sx1) * 4 + c];
                    next[((size_t)y * nw + x) * 4 + c] = (uint8_t)((sum + 2) / 4);
                }
            }
        }
        [texture replaceRegion:MTLRegionMake2D(0, 0, nw, nh)
                   mipmapLevel:mip withBytes:next.data() bytesPerRow:(NSUInteger)nw * 4];
        level = std::move(next);
        w = nw;
        h = nh;
    }
}

id<MTLTexture> createMipmapped(id<MTLDevice> device, const uint8_t *bgra,
                               uint32_t width, uint32_t height, uint64_t hash) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:width height:height mipmapped:YES];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    if (!texture) return nil;
    texture.label = [NSString stringWithFormat:@"DK2 HD %016llx", hash];
    [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
               mipmapLevel:0 withBytes:bgra bytesPerRow:(NSUInteger)width * 4];
    // CPU box-filter mip chain
    std::vector<uint8_t> level(bgra, bgra + (size_t)width * height * 4);
    uint32_t w = width, h = height;
    for (NSUInteger mip = 1; mip < texture.mipmapLevelCount; ++mip) {
        const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        std::vector<uint8_t> next((size_t)nw * nh * 4);
        for (uint32_t y = 0; y < nh; ++y) {
            const uint32_t sy0 = std::min(y * 2, h - 1), sy1 = std::min(y * 2 + 1, h - 1);
            for (uint32_t x = 0; x < nw; ++x) {
                const uint32_t sx0 = std::min(x * 2, w - 1), sx1 = std::min(x * 2 + 1, w - 1);
                for (int c = 0; c < 4; ++c) {
                    const unsigned sum = level[((size_t)sy0 * w + sx0) * 4 + c]
                                       + level[((size_t)sy0 * w + sx1) * 4 + c]
                                       + level[((size_t)sy1 * w + sx0) * 4 + c]
                                       + level[((size_t)sy1 * w + sx1) * 4 + c];
                    next[((size_t)y * nw + x) * 4 + c] = (uint8_t)((sum + 2) / 4);
                }
            }
        }
        [texture replaceRegion:MTLRegionMake2D(0, 0, nw, nh)
                   mipmapLevel:mip withBytes:next.data() bytesPerRow:(NSUInteger)nw * 4];
        level = std::move(next);
        w = nw; h = nh;
    }
    return texture;
}

id<MTLTexture> loadFile(id<MTLDevice> device, NSString *path, uint64_t hash) {
    CGImageSourceRef source = CGImageSourceCreateWithURL(
            (__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL);
    if (!source) return nil;
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, NULL);
    CFRelease(source);
    if (!image) return nil;
    const size_t width = CGImageGetWidth(image), height = CGImageGetHeight(image);
    id<MTLTexture> texture = nil;
    if (width && height && width <= 16384 && height <= 16384) {
        std::vector<uint8_t> bgra(width * height * 4);
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context = CGBitmapContextCreate(
                bgra.data(), width, height, 8, width * 4, space,
                (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        if (context) {
            CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
            CGContextRelease(context);
            // back to straight alpha - the game's blending expects it
            for (size_t i = 0; i < bgra.size(); i += 4) {
                const uint8_t a = bgra[i + 3];
                if (a == 0 || a == 255) continue;
                bgra[i] = (uint8_t)std::min(255u, bgra[i] * 255u / a);
                bgra[i + 1] = (uint8_t)std::min(255u, bgra[i + 1] * 255u / a);
                bgra[i + 2] = (uint8_t)std::min(255u, bgra[i + 2] * 255u / a);
            }
            texture = createMipmapped(device, bgra.data(),
                                      (uint32_t)width, (uint32_t)height, hash);
        }
        CGColorSpaceRelease(space);
    }
    CGImageRelease(image);
    return texture;
}

// render thread only; returns nil when no replacement exists
id<MTLTexture> lookup(id<MTLDevice> device, const uint8_t *pixels,
                      uint32_t width, uint32_t height, uint32_t pitch) {
    NSString *dir = directory();
    if (!dir) return nil;
    // Keeping every HD page ever seen resident cost ~1GB (176 files x ~5.6MB
    // as mipmapped BGRA). Evict pages not referenced for a while - they
    // reload from disk transparently on the next reference.
    struct LoadedHd {
        id<MTLTexture> texture;
        uint64_t lastUsed;
    };
    static std::unordered_map<uint64_t, LoadedHd> loaded;
    static std::unordered_set<uint64_t> missing;
    static uint64_t useTick = 0;
    ++useTick;
    if ((useTick & 1023) == 0) {
        for (auto it = loaded.begin(); it != loaded.end();) {
            // ~4096 references at a few dozen page touches per frame is on
            // the order of a couple of minutes off-screen
            if (useTick - it->second.lastUsed > 4096) it = loaded.erase(it);
            else ++it;
        }
    }
    const uint64_t hash = texdump::contentHash(pixels, width, height, pitch);
    auto found = loaded.find(hash);
    if (found != loaded.end()) {
        found->second.lastUsed = useTick;
        return found->second.texture;
    }
    if (missing.count(hash)) return nil;
    NSString *path = [dir stringByAppendingPathComponent:
            [NSString stringWithFormat:@"%016llx.png", hash]];
    id<MTLTexture> texture = access(path.fileSystemRepresentation, R_OK) == 0
            ? loadFile(device, path, hash)
            : nil;
    if (texture) {
        loaded.emplace(hash, LoadedHd{texture, useTick});
        NSLog(@"texhd: %016llx replaced with %lux%lu",
              hash, (unsigned long)texture.width, (unsigned long)texture.height);
    } else {
        if (missing.size() > 100000) missing.clear();  // dynamic frames grow this
        missing.insert(hash);
    }
    return texture;
}

}  // namespace texhd

// Named HD resource pack (protocol PAGE_ATLAS_MAP): the game reports which
// resource name was composited into which rect of which bridge texture
// ("page"). When a loose PNG named after that resource exists in the user's
// pack directory, the page is rebuilt at 4x with each mapped rect drawn from
// its pack PNG; unmapped rects fall back to bilinear-upscaled original
// pixels, so the page stays uniform. The hash-based texhd lookup above stays
// as the fallback for textures with no atlas map. All state is touched from
// the render thread only, same as texhd.
namespace respack {

constexpr uint32_t kScale = 4;
constexpr uint32_t kMaxPageDim = 1024;      // atlas pages only, not framebuffers
constexpr size_t kMaxRectsPerPage = 512;
constexpr uint32_t kMaxRebuildsPerFrame = 4;
constexpr uint32_t kDemoteRebuilds = 32;    // rect-updated too often => not pack material

struct PageState {
    std::vector<DK2MPageAtlasMapCommand> rects;  // textureId field unused here
    std::vector<uint8_t> pixels1x;               // last full upload, tight w*4 pitch
    uint32_t w = 0, h = 0;
    id<MTLTexture> hdTexture = nil;              // currently bound 4x page, nil if none
    uint32_t rebuilds = 0;                       // reset by every full upload
    bool dirty = false;
    bool demoted = false;
};

std::unordered_map<uint32_t, PageState> &pages() {
    static std::unordered_map<uint32_t, PageState> map;
    return map;
}

uint64_t &pngCacheEpoch() {
    static uint64_t epoch = 1;
    return epoch;
}

void liveStateChanged() {
    if (++pngCacheEpoch() == 0) pngCacheEpoch() = 1;
    for (auto &entry : pages()) {
        PageState &st = entry.second;
        st.dirty = true;
        st.demoted = false;
        st.rebuilds = 0;
    }
}

NSString *candidatePath() {
    static NSString *path = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *overridePath = std::getenv("DK2_RESOURCE_PACK_DIR");
        path = overridePath && *overridePath
            ? [NSString stringWithUTF8String:overridePath]
            : [NSHomeDirectory() stringByAppendingPathComponent:
                @"Library/Application Support/Dungeon Keeper II/resource-pack/textures"];
        [[NSFileManager defaultManager] createDirectoryAtPath:path
                                  withIntermediateDirectories:YES attributes:nil error:nil];
    });
    return path;
}

// Existence is re-checked per call (never dispatch_once'd) so a pack the
// user drops in mid-session is picked up -- same rule as texhd::directory().
NSString *directory() {
    if (!dk2cfg::gHdTexturesLive.load(std::memory_order_relaxed)) return nil;
    NSString *path = candidatePath();
    BOOL isDir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&isDir] || !isDir) {
        return nil;
    }
    return path;
}

// Byte-for-byte mirror of the game-side TextureDump sanitizer, so pack file
// names match dumped names exactly.
NSString *sanitizedFileName(const char *name) {
    std::string out;
    for (const char *p = name; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            out += '_';
        } else {
            out += (char)c;
        }
    }
    if (out.empty()) out = "unnamed";
    return [@(out.c_str()) stringByAppendingString:@".png"];
}

// PNG -> straight-alpha BGRA, same decode path as texhd::loadFile but into a
// CPU buffer for compositing instead of a texture.
bool loadPngBgra(NSString *path, std::vector<uint8_t> &bgra, uint32_t &w, uint32_t &h) {
    if (access(path.fileSystemRepresentation, R_OK) != 0) return false;
    CGImageSourceRef source = CGImageSourceCreateWithURL(
            (__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL);
    if (!source) return false;
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, NULL);
    CFRelease(source);
    if (!image) return false;
    const size_t width = CGImageGetWidth(image), height = CGImageGetHeight(image);
    bool ok = false;
    if (width && height && width <= 16384 && height <= 16384) {
        bgra.assign(width * height * 4, 0);
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context = CGBitmapContextCreate(
                bgra.data(), width, height, 8, width * 4, space,
                (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        if (context) {
            CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
            CGContextRelease(context);
            for (size_t i = 0; i < bgra.size(); i += 4) {
                const uint8_t a = bgra[i + 3];
                if (a == 0 || a == 255) continue;
                bgra[i] = (uint8_t)std::min(255u, bgra[i] * 255u / a);
                bgra[i + 1] = (uint8_t)std::min(255u, bgra[i + 1] * 255u / a);
                bgra[i + 2] = (uint8_t)std::min(255u, bgra[i + 2] * 255u / a);
            }
            w = (uint32_t)width;
            h = (uint32_t)height;
            ok = true;
        }
        CGColorSpaceRelease(space);
    }
    CGImageRelease(image);
    return ok;
}

void scaleBilinearBgra(const uint8_t *src, uint32_t sw, uint32_t sh, uint32_t spitch,
                       uint8_t *dst, uint32_t dw, uint32_t dh, uint32_t dpitch) {
    if (sw == dw && sh == dh) {
        for (uint32_t y = 0; y < dh; ++y) {
            std::memcpy(dst + (size_t)y * dpitch, src + (size_t)y * spitch, (size_t)dw * 4);
        }
        return;
    }
    for (uint32_t y = 0; y < dh; ++y) {
        const float fy = dh > 1 ? (float)y * (sh - 1) / (dh - 1) : 0.f;
        const uint32_t y0 = (uint32_t)fy;
        const uint32_t y1 = std::min(y0 + 1, sh - 1);
        const float ty = fy - y0;
        const uint8_t *row0 = src + (size_t)y0 * spitch;
        const uint8_t *row1 = src + (size_t)y1 * spitch;
        uint8_t *out = dst + (size_t)y * dpitch;
        for (uint32_t x = 0; x < dw; ++x) {
            const float fx = dw > 1 ? (float)x * (sw - 1) / (dw - 1) : 0.f;
            const uint32_t x0 = (uint32_t)fx;
            const uint32_t x1 = std::min(x0 + 1, sw - 1);
            const float tx = fx - x0;
            for (int c = 0; c < 4; ++c) {
                const float top = row0[x0 * 4 + c] + (row0[x1 * 4 + c] - row0[x0 * 4 + c]) * tx;
                const float bot = row1[x0 * 4 + c] + (row1[x1 * 4 + c] - row1[x0 * 4 + c]) * tx;
                out[x * 4 + c] = (uint8_t)std::min(255.f, std::max(0.f, top + (bot - top) * ty + 0.5f));
            }
        }
    }
}

// Small decoded-PNG cache: rebuilds re-read the same handful of pack files.
struct CachedPng {
    std::vector<uint8_t> bgra;
    uint32_t w = 0, h = 0;
    uint64_t tick = 0;
};

const CachedPng *packPng(NSString *dir, const char *name) {
    static std::unordered_map<std::string, CachedPng> cache;
    static uint64_t tick = 0;
    static uint64_t cachedEpoch = 0;
    if (cachedEpoch != pngCacheEpoch()) {
        cache.clear();
        cachedEpoch = pngCacheEpoch();
    }
    ++tick;
    std::string key(name);
    auto it = cache.find(key);
    if (it != cache.end()) {
        it->second.tick = tick;
        return &it->second;
    }
    // No negative cache on purpose: the user drops files in live, and this
    // only runs on page (re)builds, so the stat cost is negligible.
    NSString *path = [dir stringByAppendingPathComponent:sanitizedFileName(name)];
    CachedPng png;
    if (!loadPngBgra(path, png.bgra, png.w, png.h)) return nullptr;
    if (cache.size() >= 64) {
        auto oldest = cache.begin();
        for (auto c = cache.begin(); c != cache.end(); ++c) {
            if (c->second.tick < oldest->second.tick) oldest = c;
        }
        cache.erase(oldest);
    }
    png.tick = tick;
    return &cache.emplace(std::move(key), std::move(png)).first->second;
}

bool noteRect(const DK2MPageAtlasMapCommand &map) {
    PageState &st = pages()[map.textureId];
    for (auto &r : st.rects) {
        if (r.x == map.x && r.y == map.y) {  // slot re-composited
            if (r.w != map.w || r.h != map.h || std::strncmp(r.name, map.name, sizeof(r.name)) != 0) {
                r = map;
                st.dirty = true;
            }
            return true;
        }
    }
    if (st.rects.size() >= kMaxRectsPerPage) return false;
    st.rects.push_back(map);
    st.dirty = true;
    return true;
}

// Composes the 4x page from stored 1x pixels + pack PNGs. nil when no pack
// file applied (caller falls through to the original/hash path).
id<MTLTexture> composePage(id<MTLDevice> device, uint32_t textureId, PageState &st,
                           uint32_t *packHits) {
    NSString *dir = directory();
    if (!dir || st.pixels1x.empty() || st.rects.empty()) return nil;
    const uint32_t hdW = st.w * kScale, hdH = st.h * kScale;
    std::vector<uint8_t> hd((size_t)hdW * hdH * 4);
    scaleBilinearBgra(st.pixels1x.data(), st.w, st.h, st.w * 4,
                      hd.data(), hdW, hdH, hdW * 4);
    uint32_t applied = 0;
    for (const auto &r : st.rects) {
        if (r.x >= st.w || r.y >= st.h) continue;
        const uint32_t rw = std::min<uint32_t>(r.w, st.w - r.x);
        const uint32_t rh = std::min<uint32_t>(r.h, st.h - r.y);
        if (!rw || !rh) continue;
        const CachedPng *png = packPng(dir, r.name);
        if (!png) continue;
        scaleBilinearBgra(png->bgra.data(), png->w, png->h, png->w * 4,
                          hd.data() + ((size_t)r.y * kScale * hdW + (size_t)r.x * kScale) * 4,
                          rw * kScale, rh * kScale, hdW * 4);
        ++applied;
    }
    if (!applied) return nil;
    if (packHits) *packHits += applied;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSLog(@"DK2 resource pack: active, first page composed from %@", dir);
    });
    id<MTLTexture> tex = texhd::createMipmapped(
            device, hd.data(), hdW, hdH, 0x5245'5041'434Bull ^ textureId);
    if (tex) tex.label = [NSString stringWithFormat:@"DK2 pack page %u", textureId];
    return tex;
}

// Full-upload entry point: stores the 1x pixels and builds the HD page if
// the id has mapped rects with pack art. Called on the TEXTURE_UPDATE path.
id<MTLTexture> buildFromUpload(id<MTLDevice> device, uint32_t textureId,
                               const uint8_t *pixels, uint32_t width, uint32_t height,
                               uint32_t pitch, uint32_t *packHits,
                               uint32_t *pagesComposed) {
    auto it = pages().find(textureId);
    if (it == pages().end() || it->second.rects.empty()) return nil;
    if (width > kMaxPageDim || height > kMaxPageDim || !width || !height) return nil;
    PageState &st = it->second;
    st.w = width;
    st.h = height;
    st.pixels1x.resize((size_t)width * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(st.pixels1x.data() + (size_t)y * width * 4,
                    pixels + (size_t)y * pitch, (size_t)width * 4);
    }
    st.dirty = false;
    st.rebuilds = 0;
    st.demoted = false;
    st.hdTexture = composePage(device, textureId, st, packHits);
    if (st.hdTexture && pagesComposed) ++*pagesComposed;
    return st.hdTexture;
}

// Keep the exact 1x page current while Metal writes an original 32x32 shadow
// surface into it. The HD page is rebuilt on the first frame that no shadow
// command targets this texture.
void storeForShadow(uint32_t textureId, const uint8_t *pixels,
                    uint32_t width, uint32_t height, uint32_t pitch) {
    auto it = pages().find(textureId);
    if (it == pages().end() || it->second.rects.empty() ||
        width > kMaxPageDim || height > kMaxPageDim || !width || !height) return;
    PageState &st = it->second;
    st.w = width;
    st.h = height;
    st.pixels1x.resize((size_t)width * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(st.pixels1x.data() + (size_t)y * width * 4,
                    pixels + (size_t)y * pitch, (size_t)width * 4);
    }
    st.rebuilds = 0;
    st.demoted = false;
    st.dirty = true;
}

bool originalPage(uint32_t textureId, std::vector<uint8_t> &pixels,
                  uint32_t &width, uint32_t &height) {
    const auto it = pages().find(textureId);
    if (it == pages().end() || it->second.pixels1x.empty()) return false;
    PageState &st = it->second;
    pixels = st.pixels1x;
    width = st.w;
    height = st.h;
    // Preserve the HD state but schedule a fresh binding after live Metal
    // shadows stop targeting this page.
    st.dirty = true;
    return width && height;
}

// Rect update against a page whose HD version is currently bound: patch the
// stored 1x pixels and schedule a rebuild. Returns false when the caller
// should apply the rect to the bound texture normally.
bool applyRectToStored(uint32_t textureId, uint32_t x, uint32_t y,
                       uint32_t width, uint32_t height,
                       const uint8_t *pixels, uint32_t pitch) {
    auto it = pages().find(textureId);
    if (it == pages().end()) return false;
    PageState &st = it->second;
    if (st.pixels1x.empty()) return false;
    const bool hdBound = st.hdTexture != nil;
    if (x >= st.w || y >= st.h || width > st.w - x || height > st.h - y) return false;
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(st.pixels1x.data() + ((size_t)(y + row) * st.w + x) * 4,
                    pixels + (size_t)row * pitch, (size_t)width * 4);
    }
    st.dirty = true;
    return hdBound;
}

// End-of-commands pass: rebuild pages whose atlas map or stored content
// changed this frame. Pages rect-updated too often demote back to a plain
// 1x texture until their next full upload.
void rebuildDirty(id<MTLDevice> device, uint32_t *packHits,
                  uint32_t *pagesComposed, uint32_t *pagesRestored,
                  const std::unordered_set<uint32_t> &suspended,
                  void (^bind)(uint32_t textureId, id<MTLTexture> texture)) {
    uint32_t budget = kMaxRebuildsPerFrame;
    for (auto &kv : pages()) {
        if (!budget) break;
        PageState &st = kv.second;
        if (!st.dirty || st.pixels1x.empty() || st.demoted) continue;
        if (suspended.contains(kv.first)) continue;
        st.dirty = false;
        --budget;
        if (++st.rebuilds > kDemoteRebuilds) {
            st.demoted = true;
            id<MTLTexture> plain = texhd::createMipmapped(
                    device, st.pixels1x.data(), st.w, st.h, kv.first);
            if (plain) {
                plain.label = [NSString stringWithFormat:@"DK2 pack demoted %u", kv.first];
                st.hdTexture = nil;
                bind(kv.first, plain);
                NSLog(@"DK2 resource pack: page %u demoted (rect-updated too often)", kv.first);
            }
            continue;
        }
        id<MTLTexture> tex = composePage(device, kv.first, st, packHits);
        if (tex) {
            st.hdTexture = tex;
            if (pagesComposed) ++*pagesComposed;
            bind(kv.first, tex);
        } else if (st.hdTexture) {
            // Map/pack no longer applies (file deleted, pack disabled):
            // restore a plain page so stale HD content doesn't linger.
            id<MTLTexture> plain = texhd::createMipmapped(
                    device, st.pixels1x.data(), st.w, st.h, kv.first);
            if (plain) {
                st.hdTexture = nil;
                if (pagesRestored) ++*pagesRestored;
                bind(kv.first, plain);
            }
        }
    }
}

size_t residentPageCount() {
    size_t count = 0;
    for (const auto &entry : pages()) {
        if (entry.second.hdTexture) ++count;
    }
    return count;
}

}  // namespace respack

namespace {

constexpr NSUInteger kFramesInFlight = 3;
constexpr NSUInteger kSampleCount = 4;
std::atomic<uint64_t> gRequestedDrawableSize{0};
// Game frame size from the bridge (packed WxH); drives the view's letterbox
// aspect so widescreen game resolutions are not squeezed into 4:3.
std::atomic<uint64_t> gGameFrameSize{0};
std::atomic<uint64_t> gRenderedFrames{0};
std::atomic<uint64_t> gBridgeFramesRendered{0};
uint64_t gSelfTestFrames = 0;
NSString *gBridgePath = nil;
NSString *gGameRunnerPath = nil;
bool gStartFullscreen = false;
bool gBridgeRequired = false;
std::atomic<DK2MFileHeader *> gInputHeader{nullptr};
// Set once in -applicationDidFinishLaunching:; lets dk2cfg::gOnApplied re-
// request layout (drawable size) after a live render_scale change.
DK2MetalView *gMetalView = nil;

void publishInputState(const DK2MInputState &state) {
    DK2MFileHeader *header = gInputHeader.load(std::memory_order_acquire);
    if (!header) return;
    DK2MInputState *destination = &header->input;
    uint32_t sequence = __atomic_load_n(&destination->sequence, __ATOMIC_ACQUIRE);
    if ((sequence & 1u) != 0) ++sequence;
    __atomic_store_n(&destination->sequence, sequence + 1, __ATOMIC_RELEASE);
    std::memcpy(reinterpret_cast<uint8_t *>(destination) + sizeof(sequence),
                reinterpret_cast<const uint8_t *>(&state) + sizeof(sequence),
                sizeof(state) - sizeof(sequence));
    __atomic_store_n(&destination->sequence, sequence + 2, __ATOMIC_RELEASE);
}

uint64_t packSize(CGSize size) {
    const auto width = static_cast<uint32_t>(std::max<CGFloat>(1, std::round(size.width)));
    const auto height = static_cast<uint32_t>(std::max<CGFloat>(1, std::round(size.height)));
    return (static_cast<uint64_t>(width) << 32) | height;
}

CGSize unpackSize(uint64_t packed) {
    return CGSizeMake(static_cast<uint32_t>(packed >> 32), static_cast<uint32_t>(packed));
}

void fail(NSString *message) {
    NSLog(@"FATAL: %@", message);
    dispatch_async(dispatch_get_main_queue(), ^{
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"Dungeon Keeper II";
        alert.informativeText = message;
        [alert runModal];
        [NSApp terminate:nil];
    });
}

struct FrameSnapshot {
    uint32_t frame = 0;
    uint32_t commandCount = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sceneMicroseconds = 0;
    uint32_t tickMicroseconds = 0;
    uint32_t prepareMicroseconds = 0;
    uint32_t drawMicroseconds = 0;
    uint32_t producerDrawCopyMicroseconds = 0;
    uint32_t producerTextureMicroseconds = 0;
    uint32_t producerOverlayMicroseconds = 0;
    std::vector<uint8_t> bytes;
};

class BridgeReader {
public:
    explicit BridgeReader(const char *path) {
        descriptor_ = open(path, O_RDWR | O_CREAT, 0600);
        if (descriptor_ < 0) return;
        struct stat status = {};
        if (fstat(descriptor_, &status) != 0 || status.st_size != DK2M_FILE_SIZE) {
            if (ftruncate(descriptor_, DK2M_FILE_SIZE) != 0) return;
        }
        mapping_ = mmap(nullptr, DK2M_FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor_, 0);
        if (mapping_ == MAP_FAILED) {
            mapping_ = nullptr;
            return;
        }
        header_ = static_cast<DK2MFileHeader *>(mapping_);
        if (header_->magic != DK2M_MAGIC || header_->version != DK2M_VERSION ||
            header_->header_size != sizeof(DK2MFileHeader) || header_->file_size != DK2M_FILE_SIZE) {
            std::memset(mapping_, 0, DK2M_FILE_SIZE);
            header_->magic = DK2M_MAGIC;
            header_->version = DK2M_VERSION;
            header_->header_size = sizeof(DK2MFileHeader);
            header_->file_size = DK2M_FILE_SIZE;
            header_->latest_slot = DK2M_NO_SLOT;
        }
        uint32_t session = __atomic_add_fetch(&header_->consumer_session, 1, __ATOMIC_ACQ_REL);
        if (session == 0) __atomic_add_fetch(&header_->consumer_session, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&header_->consumer_frame, 0, __ATOMIC_RELEASE);
        gInputHeader.store(header_, std::memory_order_release);
    }

    ~BridgeReader() {
        if (header_) {
            DK2MInputState stopped = {};
            publishInputState(stopped);
        }
        DK2MFileHeader *expected = header_;
        gInputHeader.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
        if (mapping_) munmap(mapping_, DK2M_FILE_SIZE);
        if (descriptor_ >= 0) close(descriptor_);
    }

    bool valid() const { return header_ != nullptr; }

    const FrameSnapshot *poll() {
        if (!header_) return nullptr;
        const uint32_t slotIndex = __atomic_load_n(&header_->latest_slot, __ATOMIC_ACQUIRE);
        if (slotIndex >= DK2M_SLOT_COUNT) return snapshot_.frame == 0 ? nullptr : &snapshot_;

        DK2MFrameSlot *slot = &header_->slots[slotIndex];
        const uint32_t sequenceBefore = __atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE);
        if ((sequenceBefore & 1u) != 0) return snapshot_.frame == 0 ? nullptr : &snapshot_;
        const uint32_t frame = slot->frame_number;
        const uint32_t byteCount = slot->byte_count;
        if (frame == 0 || byteCount > DK2M_SLOT_CAPACITY || slot->width == 0 || slot->height == 0) {
            return snapshot_.frame == 0 ? nullptr : &snapshot_;
        }
        if (frame != snapshot_.frame) {
            FrameSnapshot &next = pending_;
            next.frame = frame;
            next.commandCount = slot->command_count;
            next.width = slot->width;
            next.height = slot->height;
            next.sceneMicroseconds =
                    (slot->game_timings[0] & 0xFFFFu) * DK2M_TIMING_QUANTUM_US;
            next.tickMicroseconds =
                    (slot->game_timings[0] >> 16) * DK2M_TIMING_QUANTUM_US;
            next.prepareMicroseconds =
                    (slot->game_timings[1] & 0xFFFFu) * DK2M_TIMING_QUANTUM_US;
            next.drawMicroseconds =
                    (slot->game_timings[1] >> 16) * DK2M_TIMING_QUANTUM_US;
            next.producerDrawCopyMicroseconds =
                    (slot->producer_timings[0] & 0xFFFFu) * DK2M_TIMING_QUANTUM_US;
            next.producerTextureMicroseconds =
                    (slot->producer_timings[0] >> 16) * DK2M_TIMING_QUANTUM_US;
            next.producerOverlayMicroseconds =
                    (slot->producer_timings[1] & 0xFFFFu) * DK2M_TIMING_QUANTUM_US;
            next.bytes.resize(byteCount);
            std::memcpy(next.bytes.data(), static_cast<uint8_t *>(mapping_) + DK2M_SLOT_OFFSET(slotIndex), byteCount);
            const uint32_t sequenceAfter = __atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE);
            if (sequenceBefore == sequenceAfter && (sequenceAfter & 1u) == 0) {
                std::swap(snapshot_, next);
                __atomic_store_n(&header_->consumer_frame, frame, __ATOMIC_RELEASE);
                if (frame <= 2 || frame % 300 == 0) {
                    NSLog(@"Bridge accepted frame %u (%u commands, %u bytes)",
                          frame, snapshot_.commandCount, byteCount);
                }
            }
        }
        return snapshot_.frame == 0 ? nullptr : &snapshot_;
    }

private:
    int descriptor_ = -1;
    void *mapping_ = nullptr;
    DK2MFileHeader *header_ = nullptr;
    FrameSnapshot snapshot_;
    FrameSnapshot pending_;
};

using TelemetryClock = std::chrono::steady_clock;

uint32_t elapsedMicroseconds(TelemetryClock::time_point start,
                             TelemetryClock::time_point end) {
    const auto value = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    return value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(std::max<int64_t>(0, value));
}

struct FrameMetrics {
    uint32_t intervalUs = 0;
    uint32_t sceneUs = 0;
    uint32_t tickUs = 0;
    uint32_t prepareUs = 0;
    uint32_t drawUs = 0;
    uint32_t bridgeBytes = 0;
    uint32_t commands = 0;
    uint32_t drawCalls = 0;
    uint32_t vertices = 0;
    uint32_t indices = 0;
    uint32_t textureUpdates = 0;
    uint32_t textureBytes = 0;
    uint32_t producerDrawCopyUs = 0;
    uint32_t producerTextureUs = 0;
    uint32_t producerOverlayUs = 0;
    uint32_t encodeUs = 0;
    uint32_t drawableWaitUs = 0;
    uint32_t gpuWaitUs = 0;
    uint32_t gpuCompleteUs = 0;
    uint32_t fvf1Draws = 0;
    uint32_t fvf2Draws = 0;
    uint32_t missingTextures = 0;
    uint32_t bindingOverflows = 0;
    uint32_t invalidDraws = 0;
    uint32_t packMapCommands = 0;
    uint32_t packMappedRects = 0;
    uint32_t packHits = 0;
    uint32_t packPagesComposed = 0;
    uint32_t packPagesRestored = 0;
    uint32_t shadowLive = 0;
    uint32_t shadowMasksReceived = 0;
    uint32_t shadowMasksRendered = 0;
    uint32_t shadowTriangles = 0;
    uint32_t shadowRejectUnavailable = 0;
    uint32_t shadowRejectMalformed = 0;
    uint32_t shadowRejectTarget = 0;
    uint32_t shadowRejectCapacity = 0;
};

class TelemetryWindow {
public:
    void add(const FrameMetrics &sample) {
        samples_[count_++] = sample;
        if (count_ != samples_.size()) return;
        NSLog(@"PERF game us p50/p95/p99: interval=%u/%u/%u tick=%u/%u/%u "
               "prepare=%u/%u/%u draw=%u/%u/%u scene=%u/%u/%u",
              p(&FrameMetrics::intervalUs, 50), p(&FrameMetrics::intervalUs, 95),
              p(&FrameMetrics::intervalUs, 99), p(&FrameMetrics::tickUs, 50),
              p(&FrameMetrics::tickUs, 95), p(&FrameMetrics::tickUs, 99),
              p(&FrameMetrics::prepareUs, 50), p(&FrameMetrics::prepareUs, 95),
              p(&FrameMetrics::prepareUs, 99), p(&FrameMetrics::drawUs, 50),
              p(&FrameMetrics::drawUs, 95), p(&FrameMetrics::drawUs, 99),
              p(&FrameMetrics::sceneUs, 50), p(&FrameMetrics::sceneUs, 95),
              p(&FrameMetrics::sceneUs, 99));
        NSLog(@"PERF bridge p50/p95/p99: bytes=%u/%u/%u commands=%u/%u/%u "
               "draws=%u/%u/%u vertices=%u/%u/%u texture-updates=%u/%u/%u "
               "texture-bytes=%u/%u/%u",
              p(&FrameMetrics::bridgeBytes, 50), p(&FrameMetrics::bridgeBytes, 95),
              p(&FrameMetrics::bridgeBytes, 99), p(&FrameMetrics::commands, 50),
              p(&FrameMetrics::commands, 95), p(&FrameMetrics::commands, 99),
              p(&FrameMetrics::drawCalls, 50), p(&FrameMetrics::drawCalls, 95),
              p(&FrameMetrics::drawCalls, 99), p(&FrameMetrics::vertices, 50),
              p(&FrameMetrics::vertices, 95), p(&FrameMetrics::vertices, 99),
              p(&FrameMetrics::textureUpdates, 50), p(&FrameMetrics::textureUpdates, 95),
              p(&FrameMetrics::textureUpdates, 99), p(&FrameMetrics::textureBytes, 50),
              p(&FrameMetrics::textureBytes, 95), p(&FrameMetrics::textureBytes, 99));
        NSLog(@"PERF host us p50/p95/p99: encode=%u/%u/%u drawable-wait=%u/%u/%u "
               "gpu-wait=%u/%u/%u gpu-complete=%u/%u/%u; diagnostics totals: "
               "fvf1=%llu fvf2=%llu missing-texture=%llu binding-overflow=%llu invalid-draw=%llu "
               "pack-maps(valid/received)=%llu/%llu pack-hits=%llu "
               "pack-pages(composed/restored)=%llu/%llu resident=%zu",
              p(&FrameMetrics::encodeUs, 50), p(&FrameMetrics::encodeUs, 95),
              p(&FrameMetrics::encodeUs, 99), p(&FrameMetrics::drawableWaitUs, 50),
              p(&FrameMetrics::drawableWaitUs, 95), p(&FrameMetrics::drawableWaitUs, 99),
              p(&FrameMetrics::gpuWaitUs, 50), p(&FrameMetrics::gpuWaitUs, 95),
              p(&FrameMetrics::gpuWaitUs, 99), p(&FrameMetrics::gpuCompleteUs, 50),
              p(&FrameMetrics::gpuCompleteUs, 95), p(&FrameMetrics::gpuCompleteUs, 99),
              total(&FrameMetrics::fvf1Draws), total(&FrameMetrics::fvf2Draws),
              total(&FrameMetrics::missingTextures), total(&FrameMetrics::bindingOverflows),
              total(&FrameMetrics::invalidDraws), total(&FrameMetrics::packMappedRects),
              total(&FrameMetrics::packMapCommands), total(&FrameMetrics::packHits),
              total(&FrameMetrics::packPagesComposed),
              total(&FrameMetrics::packPagesRestored), respack::residentPageCount());
        NSLog(@"PERF producer us p50/p95/p99: draw-copy=%u/%u/%u texture=%u/%u/%u "
               "overlay=%u/%u/%u",
              p(&FrameMetrics::producerDrawCopyUs, 50), p(&FrameMetrics::producerDrawCopyUs, 95),
              p(&FrameMetrics::producerDrawCopyUs, 99), p(&FrameMetrics::producerTextureUs, 50),
              p(&FrameMetrics::producerTextureUs, 95), p(&FrameMetrics::producerTextureUs, 99),
              p(&FrameMetrics::producerOverlayUs, 50), p(&FrameMetrics::producerOverlayUs, 95),
              p(&FrameMetrics::producerOverlayUs, 99));
        NSLog(@"PERF shadows totals/%zu frames: live=%llu received=%llu rendered=%llu "
               "triangles=%llu rejected(unavailable=%llu malformed=%llu target=%llu capacity=%llu)",
              kFrames, total(&FrameMetrics::shadowLive),
              total(&FrameMetrics::shadowMasksReceived),
              total(&FrameMetrics::shadowMasksRendered), total(&FrameMetrics::shadowTriangles),
              total(&FrameMetrics::shadowRejectUnavailable),
              total(&FrameMetrics::shadowRejectMalformed),
              total(&FrameMetrics::shadowRejectTarget),
              total(&FrameMetrics::shadowRejectCapacity));
        count_ = 0;
    }

private:
    static constexpr size_t kFrames = 300;
    using Field = uint32_t FrameMetrics::*;

    uint32_t p(Field field, size_t percentile) const {
        std::array<uint32_t, kFrames> values;
        for (size_t i = 0; i < count_; ++i) values[i] = samples_[i].*field;
        const size_t index = (count_ - 1) * percentile / 100;
        std::nth_element(values.begin(), values.begin() + index, values.begin() + count_);
        return values[index];
    }

    uint64_t total(Field field) const {
        uint64_t value = 0;
        for (size_t i = 0; i < count_; ++i) value += samples_[i].*field;
        return value;
    }

    std::array<FrameMetrics, kFrames> samples_ = {};
    size_t count_ = 0;
};

struct CommandView {
    uint16_t type;
    uint32_t offset;
    uint32_t size;
};

bool decodeAtlasMap(const FrameSnapshot *snapshot, const CommandView &view,
                    DK2MPageAtlasMapCommand &map) {
    if (!snapshot || view.type != DK2M_COMMAND_PAGE_ATLAS_MAP) return false;
    if (view.size >= sizeof(DK2MCommandHeader) + sizeof(map)) {
        std::memcpy(&map, snapshot->bytes.data() + view.offset + sizeof(DK2MCommandHeader),
                    sizeof(map));
    } else if (view.size >= sizeof(map)) {
        std::memcpy(&map, snapshot->bytes.data() + view.offset, sizeof(map));
    } else {
        return false;
    }
    map.name[sizeof(map.name) - 1] = 0;
    return map.textureId && map.w && map.h && map.name[0];
}

struct TextureBinding {
    uint16_t bank;
    uint16_t slot;
};

struct TextureBindingEntry {
    uint32_t textureId;
    TextureBinding binding;
};

struct DrawUniform {
    float screenWidth;
    float screenHeight;
    uint32_t textureIndex;
    uint32_t colorOp;
    uint32_t colorArg1;
    uint32_t colorArg2;
    uint32_t alphaOp;
    uint32_t alphaArg1;
    uint32_t alphaArg2;
    uint32_t textureFactor;
    uint32_t textureIndex1;
    uint32_t colorOp1;
    uint32_t colorArg1_1;
    uint32_t colorArg2_1;
    uint32_t alphaOp1;
    uint32_t alphaArg1_1;
    uint32_t alphaArg2_1;
    uint32_t textureIndex2;
    uint32_t colorOp2;
    uint32_t colorArg1_2;
    uint32_t colorArg2_2;
    uint32_t alphaOp2;
    uint32_t alphaArg1_2;
    uint32_t alphaArg2_2;
    // D3DTOP_BUMPENVMAP[LUMINANCE] parameters - see DK2Shaders.metal's
    // dk2_apply_bump_env. One set per stage that can perturb the next.
    float bumpEnvMat0_00;
    float bumpEnvMat0_01;
    float bumpEnvMat0_10;
    float bumpEnvMat0_11;
    float bumpEnvLScale0;
    float bumpEnvLOffset0;
    float bumpEnvMat1_00;
    float bumpEnvMat1_01;
    float bumpEnvMat1_10;
    float bumpEnvMat1_11;
    float bumpEnvLScale1;
    float bumpEnvLOffset1;
    // Metal shadows: 1 for depth-tested, depth-writing world geometry eligible
    // for shadow darkening. UI and translucent draws commonly leave Z enabled
    // but disable Z writes, so ZENABLE alone is not a scene/UI boundary.
    // Host-only bookkeeping, never comes from the wire protocol.
    uint32_t worldGeometry;
};

constexpr NSUInteger kVertexBufferSize = 2 * 1024 * 1024;
constexpr NSUInteger kIndexBufferSize = 512 * 1024;
constexpr NSUInteger kMaxDrawsPerFrame = 4096;
constexpr NSUInteger kDrawBufferSize = kMaxDrawsPerFrame * sizeof(DrawUniform);
// Metal exposes at most 128 textures through one shader argument table, so
// distinct textures needed in one frame are split across several immutable
// banks, switching the bound table when a draw references a different one.
// Slot zero in every bank is the untextured white fallback. Two banks (256
// total) covered DK2's High-Res menus, but full 3D gameplay at high zoom-out
// with many distinct per-tile reduction-level textures live measurably
// exceeds that - diagnostics showed binding-overflow climbing into the
// thousands per ~10s window, meaning draws whose texture didn't fit fell
// back to a flat white/vertex-colour look (this is what made water and
// other geometry render as a plain colour). Argument tables are cheap
// (small GPU-visible descriptor buffers, not the textures themselves, which
// are already resident in _textures regardless of bank count), so budget
// generously rather than re-tune this by trial and error per scene.
constexpr NSUInteger kTextureBindingsPerArgumentTable = 128;
constexpr NSUInteger kTextureArgumentTablesPerFrame = 48;
constexpr uint32_t kD3DRenderStateZEnable = 7;
constexpr uint32_t kD3DRenderStateZWriteEnable = 14;
constexpr uint32_t kD3DRenderStateSourceBlend = 19;
constexpr uint32_t kD3DRenderStateDestinationBlend = 20;
constexpr uint32_t kD3DRenderStateCullMode = 22;
constexpr uint32_t kD3DRenderStateZFunc = 23;
constexpr uint32_t kD3DRenderStateAlphaBlendEnable = 27;
constexpr uint32_t kD3DRenderStateTextureFactor = 60;
static_assert(sizeof(DrawUniform) == 148);

// Subtle bloom on lava/fire/torches: threshold-extract bright pixels, blur at
// half resolution, add back at low intensity. Backed by settings.toml
// [renderer] bloom (live-appliable); DK2_BLOOM=0/1 pins it for the process.
bool dk2BloomEnabled() {
    return dk2cfg::gBloomLive.load(std::memory_order_relaxed);
}

// --- world-space mesh pipeline (protocol v9) ---
// Mirrors DK2MeshDrawUniform in DK2Shaders.metal: rows of the 3x4 world
// transform plus material inputs, one per DRAW_MESH instance.
struct MeshDrawUniform {
    float world0[4];
    float world1[4];
    float world2[4];
    float ambient[4];
    uint32_t textureIndex;
    uint32_t tint;
    uint32_t flags;
    uint32_t pad;
};
static_assert(sizeof(MeshDrawUniform) == 80);
constexpr NSUInteger kMeshVertexStride = 36;  // sizeof(DK2MMeshVertex)
constexpr NSUInteger kMeshVertexBufferSize = 4 * 1024 * 1024;
constexpr NSUInteger kMaxMeshDrawsPerFrame = 8192;
constexpr NSUInteger kMeshDrawBufferSize = kMaxMeshDrawsPerFrame * sizeof(MeshDrawUniform);
// lights buffer layout: 16B header + 256-float LUT at +16, lights at +1040
constexpr NSUInteger kLightsHeaderBytes = 16 + 256 * sizeof(float);
constexpr NSUInteger kMaxLightsPerFrame = 1024;
constexpr NSUInteger kLightsBufferSize = kLightsHeaderBytes + kMaxLightsPerFrame * 48;
constexpr NSUInteger kCameraBufferSize = 24 * sizeof(float);

// Metal shadows keep DK2's original light selection, projection, blend,
// receiver geometry and decal draw. Only its 256x256-subpixel silhouette
// rasterizer is replaced with a Metal pass that resolves into the original
// 32x32 shadow-atlas slot before the scene samples it.
// Backed by settings.toml [renderer] metal_shadows (live-appliable);
// DK2_METAL_SHADOWS=0/1 pins it for the process.
bool dk2ShadowsEnabled() {
    return dk2cfg::gShadowsLive.load(std::memory_order_relaxed);
}

std::atomic_bool gShadowGpuAvailable{false};

// Relative factor of the display's full Retina backing size (i.e.
// [view convertSizeToBacking:size]) used for the drawable, and therefore
// every render target sized from it: MSAA color/depth, scene color and bloom
// half-res. Default 1.0 reproduces today's full-backing-
// scale behavior byte-for-byte on any display (1x or 2x); DK2_RENDER_SCALE
// below 1.0 asks for sub-native rendering (CAMetalLayer upscales the
// presented texture to the layer automatically). Clamped to 0.375..1.0.
//
// This used to be an absolute backing-scale-factor target (default 2.0,
// divided by the window's actual backingScaleFactor before use): correct on
// 2x Retina displays, where it canceled out to 1.0x the backing size, but on
// 1x external displays (backingScaleFactor 1) it left a factor of 2.0
// uncanceled, silently rendering at 2x the display's native pixel count.
// Backed by settings.toml [renderer] render_scale (live-appliable, re-
// requests the drawable size on change); DK2_RENDER_SCALE pins it.
float dk2RenderScale() {
    return dk2cfg::bitsFloat(dk2cfg::gRenderScaleBits.load(std::memory_order_relaxed));
}

constexpr NSUInteger kShadowSubpixelSize = 256;
constexpr NSUInteger kShadowTilesPerAxis = 8;
constexpr NSUInteger kShadowScratchSize = kShadowSubpixelSize * kShadowTilesPerAxis;
constexpr NSUInteger kMaxShadowMasksPerFrame = 64;
constexpr NSUInteger kShadowVertexBufferSize = 512 * 1024;
constexpr NSUInteger kShadowResolveBufferSize =
    kMaxShadowMasksPerFrame * 32;

struct ShadowMaskVertex {
    float x;
    float y;
};

struct ShadowResolveUniform {
    uint32_t scratchX;
    uint32_t scratchY;
    uint32_t targetX;
    uint32_t targetY;
    uint32_t mode;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};
static_assert(sizeof(ShadowResolveUniform) == 32);

MTLCompareFunction metalCompareFunction(uint32_t d3dFunction) {
    switch (d3dFunction) {
        case 1: return MTLCompareFunctionNever;
        case 2: return MTLCompareFunctionLess;
        case 3: return MTLCompareFunctionEqual;
        case 4: return MTLCompareFunctionLessEqual;
        case 5: return MTLCompareFunctionGreater;
        case 6: return MTLCompareFunctionNotEqual;
        case 7: return MTLCompareFunctionGreaterEqual;
        case 8: return MTLCompareFunctionAlways;
        default: return MTLCompareFunctionLessEqual;
    }
}

uint8_t dikForKeyCode(GCKeyCode code) {
#define DK2_MAP_KEY(name, dik) if (code == GCKeyCode##name) return dik
    DK2_MAP_KEY(Escape, 0x01);
    DK2_MAP_KEY(One, 0x02); DK2_MAP_KEY(Two, 0x03); DK2_MAP_KEY(Three, 0x04);
    DK2_MAP_KEY(Four, 0x05); DK2_MAP_KEY(Five, 0x06); DK2_MAP_KEY(Six, 0x07);
    DK2_MAP_KEY(Seven, 0x08); DK2_MAP_KEY(Eight, 0x09); DK2_MAP_KEY(Nine, 0x0A);
    DK2_MAP_KEY(Zero, 0x0B); DK2_MAP_KEY(Hyphen, 0x0C); DK2_MAP_KEY(EqualSign, 0x0D);
    DK2_MAP_KEY(DeleteOrBackspace, 0x0E); DK2_MAP_KEY(Tab, 0x0F);
    DK2_MAP_KEY(KeyQ, 0x10); DK2_MAP_KEY(KeyW, 0x11); DK2_MAP_KEY(KeyE, 0x12);
    DK2_MAP_KEY(KeyR, 0x13); DK2_MAP_KEY(KeyT, 0x14); DK2_MAP_KEY(KeyY, 0x15);
    DK2_MAP_KEY(KeyU, 0x16); DK2_MAP_KEY(KeyI, 0x17); DK2_MAP_KEY(KeyO, 0x18);
    DK2_MAP_KEY(KeyP, 0x19); DK2_MAP_KEY(OpenBracket, 0x1A); DK2_MAP_KEY(CloseBracket, 0x1B);
    DK2_MAP_KEY(ReturnOrEnter, 0x1C); DK2_MAP_KEY(LeftControl, 0x1D);
    DK2_MAP_KEY(KeyA, 0x1E); DK2_MAP_KEY(KeyS, 0x1F); DK2_MAP_KEY(KeyD, 0x20);
    DK2_MAP_KEY(KeyF, 0x21); DK2_MAP_KEY(KeyG, 0x22); DK2_MAP_KEY(KeyH, 0x23);
    DK2_MAP_KEY(KeyJ, 0x24); DK2_MAP_KEY(KeyK, 0x25); DK2_MAP_KEY(KeyL, 0x26);
    DK2_MAP_KEY(Semicolon, 0x27); DK2_MAP_KEY(Quote, 0x28);
    DK2_MAP_KEY(GraveAccentAndTilde, 0x29); DK2_MAP_KEY(LeftShift, 0x2A);
    DK2_MAP_KEY(Backslash, 0x2B); DK2_MAP_KEY(KeyZ, 0x2C); DK2_MAP_KEY(KeyX, 0x2D);
    DK2_MAP_KEY(KeyC, 0x2E); DK2_MAP_KEY(KeyV, 0x2F); DK2_MAP_KEY(KeyB, 0x30);
    DK2_MAP_KEY(KeyN, 0x31); DK2_MAP_KEY(KeyM, 0x32); DK2_MAP_KEY(Comma, 0x33);
    DK2_MAP_KEY(Period, 0x34); DK2_MAP_KEY(Slash, 0x35); DK2_MAP_KEY(RightShift, 0x36);
    DK2_MAP_KEY(KeypadAsterisk, 0x37); DK2_MAP_KEY(LeftAlt, 0x38); DK2_MAP_KEY(Spacebar, 0x39);
    DK2_MAP_KEY(CapsLock, 0x3A); DK2_MAP_KEY(F1, 0x3B); DK2_MAP_KEY(F2, 0x3C);
    DK2_MAP_KEY(F3, 0x3D); DK2_MAP_KEY(F4, 0x3E); DK2_MAP_KEY(F5, 0x3F);
    DK2_MAP_KEY(F6, 0x40); DK2_MAP_KEY(F7, 0x41); DK2_MAP_KEY(F8, 0x42);
    DK2_MAP_KEY(F9, 0x43); DK2_MAP_KEY(F10, 0x44); DK2_MAP_KEY(KeypadNumLock, 0x45);
    DK2_MAP_KEY(ScrollLock, 0x46); DK2_MAP_KEY(Keypad7, 0x47); DK2_MAP_KEY(Keypad8, 0x48);
    DK2_MAP_KEY(Keypad9, 0x49); DK2_MAP_KEY(KeypadHyphen, 0x4A); DK2_MAP_KEY(Keypad4, 0x4B);
    DK2_MAP_KEY(Keypad5, 0x4C); DK2_MAP_KEY(Keypad6, 0x4D); DK2_MAP_KEY(KeypadPlus, 0x4E);
    DK2_MAP_KEY(Keypad1, 0x4F); DK2_MAP_KEY(Keypad2, 0x50); DK2_MAP_KEY(Keypad3, 0x51);
    DK2_MAP_KEY(Keypad0, 0x52); DK2_MAP_KEY(KeypadPeriod, 0x53);
    DK2_MAP_KEY(F11, 0x57); DK2_MAP_KEY(F12, 0x58); DK2_MAP_KEY(KeypadEnter, 0x9C);
    DK2_MAP_KEY(RightControl, 0x9D); DK2_MAP_KEY(KeypadSlash, 0xB5);
    DK2_MAP_KEY(PrintScreen, 0xB7); DK2_MAP_KEY(RightAlt, 0xB8); DK2_MAP_KEY(Pause, 0xC5);
    DK2_MAP_KEY(Home, 0xC7); DK2_MAP_KEY(UpArrow, 0xC8); DK2_MAP_KEY(PageUp, 0xC9);
    DK2_MAP_KEY(LeftArrow, 0xCB); DK2_MAP_KEY(RightArrow, 0xCD); DK2_MAP_KEY(End, 0xCF);
    DK2_MAP_KEY(DownArrow, 0xD0); DK2_MAP_KEY(PageDown, 0xD1); DK2_MAP_KEY(Insert, 0xD2);
    DK2_MAP_KEY(DeleteForward, 0xD3); DK2_MAP_KEY(LeftGUI, 0xDB); DK2_MAP_KEY(RightGUI, 0xDC);
    DK2_MAP_KEY(Application, 0xDD);
#undef DK2_MAP_KEY
    return 0;
}

// NSEvent uses the hardware-position virtual key codes from HIToolbox.  Keep
// this as a fallback for machines where GameController doesn't publish the
// built-in keyboard; only one path is active so a key never reaches DK2 twice.
uint8_t dikForMacKeyCode(unsigned short code) {
    switch (code) {
    case 53: return 0x01; // Escape
    case 18: return 0x02; case 19: return 0x03; case 20: return 0x04;
    case 21: return 0x05; case 23: return 0x06; case 22: return 0x07;
    case 26: return 0x08; case 28: return 0x09; case 25: return 0x0A;
    case 29: return 0x0B; case 27: return 0x0C; case 24: return 0x0D;
    case 51: return 0x0E; case 48: return 0x0F;
    case 12: return 0x10; case 13: return 0x11; case 14: return 0x12;
    case 15: return 0x13; case 17: return 0x14; case 16: return 0x15;
    case 32: return 0x16; case 34: return 0x17; case 31: return 0x18;
    case 35: return 0x19; case 33: return 0x1A; case 30: return 0x1B;
    case 36: return 0x1C; case 59: return 0x1D;
    case 0: return 0x1E; case 1: return 0x1F; case 2: return 0x20;
    case 3: return 0x21; case 5: return 0x22; case 4: return 0x23;
    case 38: return 0x24; case 40: return 0x25; case 37: return 0x26;
    case 41: return 0x27; case 39: return 0x28; case 50: return 0x29;
    case 56: return 0x2A; case 42: return 0x2B;
    case 6: return 0x2C; case 7: return 0x2D; case 8: return 0x2E;
    case 9: return 0x2F; case 11: return 0x30; case 45: return 0x31;
    case 46: return 0x32; case 43: return 0x33; case 47: return 0x34;
    case 44: return 0x35; case 60: return 0x36; case 67: return 0x37;
    case 58: return 0x38; case 49: return 0x39; case 57: return 0x3A;
    case 122: return 0x3B; case 120: return 0x3C; case 99: return 0x3D;
    case 118: return 0x3E; case 96: return 0x3F; case 97: return 0x40;
    case 98: return 0x41; case 100: return 0x42; case 101: return 0x43;
    case 109: return 0x44; case 71: return 0x45;
    case 89: return 0x47; case 91: return 0x48; case 92: return 0x49;
    case 78: return 0x4A; case 86: return 0x4B; case 87: return 0x4C;
    case 88: return 0x4D; case 69: return 0x4E; case 83: return 0x4F;
    case 84: return 0x50; case 85: return 0x51; case 82: return 0x52;
    case 65: return 0x53; case 103: return 0x57; case 111: return 0x58;
    case 76: return 0x9C; case 62: return 0x9D; case 75: return 0xB5;
    case 61: return 0xB8; case 115: return 0xC7; case 126: return 0xC8;
    case 116: return 0xC9; case 123: return 0xCB; case 124: return 0xCD;
    case 119: return 0xCF; case 125: return 0xD0; case 121: return 0xD1;
    case 114: return 0xD2; case 117: return 0xD3;
    case 55: return 0xDB; case 54: return 0xDC;
    default: return 0;
    }
}

NSEventModifierFlags modifierMaskForMacKeyCode(unsigned short code) {
    switch (code) {
    case 56: case 60: return NSEventModifierFlagShift;
    case 59: case 62: return NSEventModifierFlagControl;
    case 58: case 61: return NSEventModifierFlagOption;
    case 54: case 55: return NSEventModifierFlagCommand;
    case 57: return NSEventModifierFlagCapsLock;
    default: return 0;
    }
}

bool inputLogEnabled() {
    static const bool enabled = std::getenv("DK2_METAL_INPUT_LOG") != nullptr;
    return enabled;
}

} // namespace

@interface DK2MetalView : NSView
@property(nonatomic, readonly) CAMetalLayer *metalLayer;
- (void)setInputActive:(BOOL)active;
- (void)publishCurrentInput;
- (void)configureInputDevices;
@end

@implementation DK2MetalView {
    CAMetalLayer *_metalLayer;
    NSTrackingArea *_trackingArea;
    GCMouse *_mouse;
    GCKeyboard *_keyboard;
    DK2MInputState _input;
    NSTimer *_heartbeatTimer;
}

- (CALayer *)makeBackingLayer {
    CALayer *layer = [CALayer layer];
    layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    return layer;
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
        _metalLayer = [CAMetalLayer layer];
        _metalLayer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        [self.layer addSublayer:_metalLayer];
        std::memset(&_input, 0, sizeof(_input));
        _input.host_pid = getpid();
        [self configureInputDevices];
        __weak DK2MetalView *weakSelf = self;
        _heartbeatTimer = [NSTimer scheduledTimerWithTimeInterval:0.1
                                                         repeats:YES
                                                           block:^(NSTimer *timer) {
            (void)timer;
            DK2MetalView *view = weakSelf;
            if (!view) return;
            ++view->_input.heartbeat;
            [view publishCurrentInput];
        }];
    }
    return self;
}

- (void)dealloc {
    [_heartbeatTimer invalidate];
    _mouse.mouseInput.mouseMovedHandler = nil;
    _keyboard.keyboardInput.keyChangedHandler = nil;
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (CAMetalLayer *)metalLayer {
    return _metalLayer;
}

- (void)publishCurrentInput {
    DK2MInputState published = _input;
    if (dk2ShadowsEnabled() && gShadowGpuAvailable.load(std::memory_order_acquire)) {
        published.flags |= DK2M_INPUT_METAL_SHADOWS;
    } else {
        published.flags &= ~DK2M_INPUT_METAL_SHADOWS;
    }
    publishInputState(published);
}

- (void)appendInputEvent:(uint16_t)type code:(uint16_t)code value:(int32_t)value {
    const uint32_t write = ++_input.event_write;
    DK2MInputEvent *event =
        &_input.events[(write - 1) % DK2M_INPUT_EVENT_CAPACITY];
    event->type = type;
    event->code = code;
    event->value = value;
}

- (void)setKey:(uint8_t)dik pressed:(BOOL)pressed {
    if (dik == 0) return;
    const uint8_t mask = static_cast<uint8_t>(1u << (dik & 7));
    uint8_t *byte = &_input.keys[dik >> 3];
    const BOOL wasPressed = (*byte & mask) != 0;
    if (wasPressed == pressed) return;
    if (pressed) *byte |= mask;
    else *byte &= static_cast<uint8_t>(~mask);
    if (inputLogEnabled()) NSLog(@"Input key DIK 0x%02x %@", dik, pressed ? @"down" : @"up");
    [self appendInputEvent:DK2M_INPUT_EVENT_KEY code:dik value:pressed ? 1 : 0];
    [self publishCurrentInput];
}

- (void)setButton:(uint16_t)button pressed:(BOOL)pressed doubleClick:(BOOL)doubleClick {
    if (button >= 4) return;
    const uint32_t mask = 1u << button;
    const BOOL wasPressed = (_input.buttons & mask) != 0;
    if (pressed) _input.buttons |= mask;
    else _input.buttons &= ~mask;
    if (wasPressed != pressed || doubleClick) {
        [self appendInputEvent:DK2M_INPUT_EVENT_BUTTON
                         code:button
                        value:doubleClick ? 2 : (pressed ? 1 : 0)];
        [self publishCurrentInput];
    }
}

- (void)attachMouse:(GCMouse *)mouse {
    if (_mouse == mouse) return;
    _mouse.mouseInput.mouseMovedHandler = nil;
    _mouse = mouse;
    if (!_mouse) return;
    _mouse.handlerQueue = dispatch_get_main_queue();
    __weak DK2MetalView *weakSelf = self;
    _mouse.mouseInput.mouseMovedHandler = ^(GCMouseInput *input, float deltaX, float deltaY) {
        (void)input;
        DK2MetalView *view = weakSelf;
        if (!view || (view->_input.flags & DK2M_INPUT_ACTIVE) == 0) return;
        view->_input.relative_x += static_cast<uint32_t>(lrintf(deltaX));
        view->_input.relative_y += static_cast<uint32_t>(lrintf(-deltaY));
        [view publishCurrentInput];
    };
}

- (void)attachKeyboard:(GCKeyboard *)keyboard {
    if (_keyboard == keyboard) return;
    _keyboard.keyboardInput.keyChangedHandler = nil;
    _keyboard = keyboard;
    if (!_keyboard) return;
    _keyboard.handlerQueue = dispatch_get_main_queue();
    __weak DK2MetalView *weakSelf = self;
    _keyboard.keyboardInput.keyChangedHandler =
        ^(GCKeyboardInput *input, GCDeviceButtonInput *key, GCKeyCode code, BOOL pressed) {
            (void)input;
            (void)key;
            DK2MetalView *view = weakSelf;
            if (!view || (view->_input.flags & DK2M_INPUT_ACTIVE) == 0) return;
            [view setKey:dikForKeyCode(code) pressed:pressed];
        };
}

- (void)configureInputDevices {
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserver:self selector:@selector(mouseConnected:) name:GCMouseDidConnectNotification object:nil];
    [center addObserver:self selector:@selector(mouseDisconnected:) name:GCMouseDidDisconnectNotification object:nil];
    [center addObserver:self selector:@selector(mouseConnected:) name:GCMouseDidBecomeCurrentNotification object:nil];
    [center addObserver:self selector:@selector(keyboardConnected:) name:GCKeyboardDidConnectNotification object:nil];
    [center addObserver:self selector:@selector(keyboardDisconnected:) name:GCKeyboardDidDisconnectNotification object:nil];
    [self attachMouse:GCMouse.current ?: GCMouse.mice.firstObject];
    [self attachKeyboard:GCKeyboard.coalescedKeyboard];
}

- (void)mouseConnected:(NSNotification *)notification {
    [self attachMouse:static_cast<GCMouse *>(notification.object)];
}

- (void)mouseDisconnected:(NSNotification *)notification {
    if (notification.object == _mouse) [self attachMouse:GCMouse.current ?: GCMouse.mice.firstObject];
}

- (void)keyboardConnected:(NSNotification *)notification {
    [self attachKeyboard:static_cast<GCKeyboard *>(notification.object)];
}

- (void)keyboardDisconnected:(NSNotification *)notification {
    if (notification.object == _keyboard) [self attachKeyboard:GCKeyboard.coalescedKeyboard];
}

- (BOOL)updatePointerFromEvent:(NSEvent *)event {
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    const CGRect frame = _metalLayer.frame;
    const BOOL valid = (_input.flags & DK2M_INPUT_ACTIVE) != 0 &&
                       CGRectContainsPoint(frame, point) &&
                       CGRectGetWidth(frame) > 0 && CGRectGetHeight(frame) > 0;
    if (valid) {
        _input.cursor_x = std::clamp<float>((point.x - CGRectGetMinX(frame)) / CGRectGetWidth(frame), 0.0f, 1.0f);
        _input.cursor_y = std::clamp<float>(1.0f - (point.y - CGRectGetMinY(frame)) / CGRectGetHeight(frame), 0.0f, 1.0f);
        _input.flags |= DK2M_INPUT_CURSOR_VALID;
    } else {
        _input.flags &= ~DK2M_INPUT_CURSOR_VALID;
    }
    [self publishCurrentInput];
    return valid;
}

- (void)setInputActive:(BOOL)active {
    if (active) {
        _input.flags |= DK2M_INPUT_ACTIVE;
    } else {
        _input.flags &= ~(DK2M_INPUT_ACTIVE | DK2M_INPUT_CURSOR_VALID);
        _input.buttons = 0;
        std::memset(_input.keys, 0, sizeof(_input.keys));
    }
    [self publishCurrentInput];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea) [self removeTrackingArea:_trackingArea];
    const NSTrackingAreaOptions options = NSTrackingMouseEnteredAndExited |
                                          NSTrackingMouseMoved |
                                          NSTrackingActiveInKeyWindow |
                                          NSTrackingInVisibleRect |
                                          NSTrackingEnabledDuringMouseDrag;
    _trackingArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                 options:options
                                                   owner:self
                                                userInfo:nil];
    [self addTrackingArea:_trackingArea];
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    self.window.acceptsMouseMovedEvents = YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)event {
    (void)event;
    return YES;
}

- (void)mouseMoved:(NSEvent *)event { [self updatePointerFromEvent:event]; }
- (void)mouseDragged:(NSEvent *)event { [self updatePointerFromEvent:event]; }
- (void)rightMouseDragged:(NSEvent *)event { [self updatePointerFromEvent:event]; }
- (void)otherMouseDragged:(NSEvent *)event { [self updatePointerFromEvent:event]; }
- (void)mouseExited:(NSEvent *)event { [self updatePointerFromEvent:event]; }

- (void)scrollWheel:(NSEvent *)event {
    if (![self updatePointerFromEvent:event]) return;
    const CGFloat scale = event.hasPreciseScrollingDeltas ? 10.0 : 120.0;
    const long wheelX = std::clamp<long>(lrint(event.scrollingDeltaX * scale), -120, 120);
    const long wheelY = std::clamp<long>(lrint(event.scrollingDeltaY * scale), -120, 120);
    _input.wheel_x += static_cast<uint32_t>(wheelX);
    _input.wheel_y += static_cast<uint32_t>(wheelY);
    [self publishCurrentInput];
}

- (void)mouseDown:(NSEvent *)event {
    // acceptsFirstMouse lets the click reach us while the window is inactive,
    // but windowDidBecomeKey may run afterwards.  Arm the bridge here so the
    // activation click is not discarded.
    if ((_input.flags & DK2M_INPUT_ACTIVE) == 0) [self setInputActive:YES];
    if ([self updatePointerFromEvent:event])
        [self setButton:0 pressed:YES doubleClick:event.clickCount >= 2];
}
- (void)mouseUp:(NSEvent *)event {
    [self updatePointerFromEvent:event];
    [self setButton:0 pressed:NO doubleClick:NO];
}
- (void)rightMouseDown:(NSEvent *)event {
    if ((_input.flags & DK2M_INPUT_ACTIVE) == 0) [self setInputActive:YES];
    if ([self updatePointerFromEvent:event])
        [self setButton:1 pressed:YES doubleClick:event.clickCount >= 2];
}
- (void)rightMouseUp:(NSEvent *)event {
    [self updatePointerFromEvent:event];
    [self setButton:1 pressed:NO doubleClick:NO];
}
- (void)otherMouseDown:(NSEvent *)event {
    if ((_input.flags & DK2M_INPUT_ACTIVE) == 0) [self setInputActive:YES];
    if ([self updatePointerFromEvent:event])
        [self setButton:event.buttonNumber == 2 ? 2 : 3
                 pressed:YES
             doubleClick:event.clickCount >= 2];
}
- (void)otherMouseUp:(NSEvent *)event {
    [self updatePointerFromEvent:event];
    [self setButton:event.buttonNumber == 2 ? 2 : 3 pressed:NO doubleClick:NO];
}

- (void)keyDown:(NSEvent *)event {
    // GameController supplies low-latency scan codes, while AppKit is the
    // authoritative source for translated text. setKey de-duplicates the two
    // paths, and WM_CHAR events keep DK2 text fields (save names, chat) usable.
    [self setKey:dikForMacKeyCode(event.keyCode) pressed:YES];
    if ((event.modifierFlags & NSEventModifierFlagCommand) != 0) return;
    NSString *characters = event.characters;
    for (NSUInteger index = 0; index < characters.length; ++index) {
        unichar character = [characters characterAtIndex:index];
        if (character == 0x7F) character = 0x08; // Cocoa delete -> WM_CHAR backspace
        if (character < 0x20 && character != 0x08 && character != 0x09 &&
            character != 0x0D && character != 0x1B) continue;
        [self appendInputEvent:DK2M_INPUT_EVENT_CHAR code:0 value:character];
    }
    [self publishCurrentInput];
}

- (void)keyUp:(NSEvent *)event {
    [self setKey:dikForMacKeyCode(event.keyCode) pressed:NO];
}

- (void)flagsChanged:(NSEvent *)event {
    const NSEventModifierFlags mask = modifierMaskForMacKeyCode(event.keyCode);
    if (mask != 0) [self setKey:dikForMacKeyCode(event.keyCode)
                        pressed:(event.modifierFlags & mask) != 0];
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)resetCursorRects {
    [super resetCursorRects];
    [self addCursorRect:_metalLayer.frame cursor:NSCursor.arrowCursor];
}

- (void)layout {
    [super layout];
    const CGRect bounds = self.bounds;
    CGFloat targetAspect = 4.0 / 3.0;
    const uint64_t gameSize = gGameFrameSize.load(std::memory_order_acquire);
    if (gameSize) {
        const CGSize size = unpackSize(gameSize);
        if (size.width > 0 && size.height > 0) targetAspect = size.width / size.height;
    }
    CGFloat width = CGRectGetWidth(bounds);
    CGFloat height = width / targetAspect;
    if (height > CGRectGetHeight(bounds)) {
        height = CGRectGetHeight(bounds);
        width = height * targetAspect;
    }
    const CGRect frame = CGRectMake(
        CGRectGetMidX(bounds) - width * 0.5,
        CGRectGetMidY(bounds) - height * 0.5,
        width,
        height);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _metalLayer.frame = frame;
    _metalLayer.contentsScale = self.window.backingScaleFactor;
    [CATransaction commit];
    [self.window invalidateCursorRectsForView:self];
    const NSSize backingSize = [self convertSizeToBacking:NSMakeSize(width, height)];
    const CGFloat renderScale = dk2RenderScale();
    gRequestedDrawableSize.store(
        packSize(CGSizeMake(backingSize.width * renderScale, backingSize.height * renderScale)),
        std::memory_order_release);
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self setNeedsLayout:YES];
}

@end

@interface DK2MetalRenderer : NSObject <CAMetalDisplayLinkDelegate>
- (instancetype)initWithLayer:(CAMetalLayer *)layer;
- (void)start;
@end

@implementation DK2MetalRenderer {
    CAMetalLayer *_layer;
    CAMetalDisplayLink *_displayLink;
    id<MTLDevice> _device;
    id<MTL4CommandQueue> _queue;
    id<MTLSharedEvent> _completed;
    id<MTLRenderPipelineState> _opaquePipelines[2];
    id<MTLRenderPipelineState> _alphaPipelines[2];
    id<MTLRenderPipelineState> _additivePipelines[2];
    id<MTLDepthStencilState> _depthStates[9][2];
    id<MTLSamplerState> _sampler;
    id<MTLSamplerState> _bloomSampler;
    id<MTLTexture> _whiteTexture;
    id<MTLTexture> _multisampleColorTexture;
    id<MTLTexture> _depthTexture;
    // Bloom chain (see dk2BloomEnabled): the multisample color resolves into
    // _sceneColorTexture instead of straight to the drawable, gets
    // threshold-extracted + blurred at half resolution in _bloomTextureA/B
    // (ping-ponged), then composited additively onto the drawable in a final
    // pass. All nil/unused when the feature is disabled.
    id<MTLTexture> _sceneColorTexture;
    id<MTLTexture> _bloomTextureA;
    id<MTLTexture> _bloomTextureB;
    id<MTLRenderPipelineState> _bloomThresholdPipeline;
    id<MTLRenderPipelineState> _bloomBlurHorizontalPipeline;
    id<MTLRenderPipelineState> _bloomBlurVerticalPipeline;
    id<MTLRenderPipelineState> _bloomCompositePipeline;
    id<MTL4ArgumentTable> _bloomArgumentTables[kFramesInFlight];
    // Tracks whether bloom setup actually succeeded at runtime (metallib
    // missing a function, pipeline/argument-table creation failing, etc.).
    // Any such failure logs loudly and clears this instead of failing the
    // whole renderer init - bloom is a cosmetic extra, never load-bearing.
    BOOL _bloomAvailable;
    std::unordered_map<uint32_t, id<MTLTexture>> _textures;
    id<MTLResidencySet> _resources;
    id<MTL4CommandAllocator> _allocators[kFramesInFlight];
    id<MTL4CommandBuffer> _commandBuffers[kFramesInFlight];
    id<MTLBuffer> _vertexBuffers[kFramesInFlight];
    id<MTLBuffer> _indexBuffers[kFramesInFlight];
    id<MTLBuffer> _drawBuffers[kFramesInFlight];
    // world-space mesh pipeline (protocol v9)
    id<MTLRenderPipelineState> _meshOpaquePipeline;
    id<MTLRenderPipelineState> _meshAlphaPipeline;
    id<MTLRenderPipelineState> _meshAdditivePipeline;
    id<MTLBuffer> _meshVertexBuffers[kFramesInFlight];
    id<MTLBuffer> _meshDrawBuffers[kFramesInFlight];
    id<MTLBuffer> _lightsBuffers[kFramesInFlight];
    id<MTLBuffer> _cameraBuffers[kFramesInFlight];
    // Original DK2 shadow masks, rasterized in a 256x256-subpixel scratch
    // atlas and resolved into their normal 32x32 texture-atlas regions.
    id<MTLRenderPipelineState> _shadowMaskPipeline;
    id<MTLRenderPipelineState> _shadowResolvePipeline;
    id<MTLTexture> _shadowScratchTextures[kFramesInFlight];
    id<MTLBuffer> _shadowVertexBuffers[kFramesInFlight];
    id<MTLBuffer> _shadowResolveBuffers[kFramesInFlight];
    id<MTL4ArgumentTable> _shadowMaskArgumentTables[kFramesInFlight];
    id<MTL4ArgumentTable> _shadowResolveArgumentTables[kFramesInFlight];
    BOOL _shadowsAvailable;
    struct MeshBlob {
        std::vector<uint8_t> vertices;
        std::vector<uint8_t> indices;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
    };
    std::unordered_map<uint32_t, MeshBlob> _meshes;
    id<MTL4ArgumentTable>
        _argumentTables[kFramesInFlight][kTextureArgumentTablesPerFrame];
    std::unique_ptr<BridgeReader> _bridge;
    // Animated surfaces (torches, the heart) stream a new frame every game
    // tick. After enough consecutive HD-lookup misses the id is flagged
    // dynamic: no more content hashing, and updates rotate through a small
    // texture ring so the CPU never writes into a texture a frame in flight
    // still reads.
    struct DynamicTexture {
        uint32_t misses = 0;
        uint32_t retry = 0;   // periodic HD re-probe while classified dynamic
        uint32_t sourceWidth = 0;
        uint32_t sourceHeight = 0;
        uint8_t ringIndex = 0;
        bool dynamic = false;
        id<MTLTexture> ring[kFramesInFlight] = {};
    };
    std::unordered_map<uint32_t, DynamicTexture> _dynamicTextures;
    struct RetiredAllocation {
        id<MTLAllocation> allocation;
        uint64_t afterValue;
    };
    std::vector<RetiredAllocation> _retiredAllocations;
    std::vector<CommandView> _commandViews;
    std::vector<TextureBindingEntry> _frameTextureBindings;
    TelemetryWindow _telemetry;
    TelemetryClock::time_point _lastBridgeArrival;
    TelemetryClock::time_point _submittedAt[kFramesInFlight];
    uint64_t _submittedValue[kFramesInFlight];
    uint64_t _frame;
    uint64_t _appliedDrawableSize;
    uint32_t _lastBridgeFrame;
    BOOL _hdTexturesLive;
}

- (instancetype)initWithLayer:(CAMetalLayer *)layer {
    self = [super init];
    if (!self) return nil;

    _layer = layer;
    _device = MTLCreateSystemDefaultDevice();
    if (!_device || ![_device supportsFamily:MTLGPUFamilyMetal4]) {
        fail(@"This build needs an Apple GPU with Metal 4 on macOS 26 or newer.");
        return nil;
    }

    _layer.device = _device;
    _layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    _layer.maximumDrawableCount = 2;
    _layer.framebufferOnly = YES;
    _layer.opaque = YES;
    _layer.displaySyncEnabled = YES;
    _layer.allowsNextDrawableTimeout = YES;

    _queue = [_device newMTL4CommandQueue];
    _completed = [_device newSharedEvent];
    if (!_queue || !_completed) {
        fail(@"Metal 4 command queue creation failed.");
        return nil;
    }
    [_queue addResidencySet:_layer.residencySet];

    NSError *error = nil;
    NSURL *libraryURL = [NSBundle.mainBundle URLForResource:@"DK2Shaders" withExtension:@"metallib"];
    id<MTLLibrary> library = libraryURL ? [_device newLibraryWithURL:libraryURL error:&error] : nil;
    id<MTLFunction> vertexFunctions[] = {
        [library newFunctionWithName:@"dk2_vertex_1c"],
        [library newFunctionWithName:@"dk2_vertex_2c"],
    };
    id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"dk2_fragment"];
    MTLRenderPipelineDescriptor *pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineDescriptor.label = @"DK2 fixed-function base pipeline";
    pipelineDescriptor.fragmentFunction = fragmentFunction;
    pipelineDescriptor.colorAttachments[0].pixelFormat = _layer.pixelFormat;
    pipelineDescriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    pipelineDescriptor.rasterSampleCount = kSampleCount;
    for (NSUInteger vertexType = 0; vertexType < 2; ++vertexType) {
        pipelineDescriptor.vertexFunction = vertexFunctions[vertexType];
        pipelineDescriptor.colorAttachments[0].blendingEnabled = NO;
        _opaquePipelines[vertexType] = vertexFunctions[vertexType] && fragmentFunction
            ? [_device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error] : nil;
        pipelineDescriptor.colorAttachments[0].blendingEnabled = YES;
        pipelineDescriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        pipelineDescriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        pipelineDescriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        pipelineDescriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        _alphaPipelines[vertexType] = vertexFunctions[vertexType] && fragmentFunction
            ? [_device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error] : nil;
        pipelineDescriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        pipelineDescriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
        pipelineDescriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        pipelineDescriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        _additivePipelines[vertexType] = vertexFunctions[vertexType] && fragmentFunction
            ? [_device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error] : nil;
    }
    if (!_opaquePipelines[0] || !_opaquePipelines[1] ||
        !_alphaPipelines[0] || !_alphaPipelines[1] ||
        !_additivePipelines[0] || !_additivePipelines[1]) {
        fail([NSString stringWithFormat:@"Metal shader pipeline failed: %@", error.localizedDescription ?: @"library missing"]);
        return nil;
    }

    id<MTLFunction> meshVertexFunction = [library newFunctionWithName:@"dk2_vertex_mesh"];
    pipelineDescriptor.label = @"DK2 world-space mesh pipeline";
    pipelineDescriptor.vertexFunction = meshVertexFunction;
    pipelineDescriptor.colorAttachments[0].blendingEnabled = NO;
    _meshOpaquePipeline = meshVertexFunction && fragmentFunction
        ? [_device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error] : nil;
    pipelineDescriptor.colorAttachments[0].blendingEnabled = YES;
    pipelineDescriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pipelineDescriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipelineDescriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pipelineDescriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    _meshAlphaPipeline = meshVertexFunction && fragmentFunction
        ? [_device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error] : nil;
    pipelineDescriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    pipelineDescriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
    pipelineDescriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pipelineDescriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
    _meshAdditivePipeline = meshVertexFunction && fragmentFunction
        ? [_device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error] : nil;
    if (!_meshOpaquePipeline || !_meshAlphaPipeline || !_meshAdditivePipeline) {
        fail([NSString stringWithFormat:@"Metal mesh pipeline failed: %@", error.localizedDescription ?: @"library missing"]);
        return nil;
    }

    {
        // Always created regardless of the current metal_shadows setting, so
        // toggling it back on live (settings.toml/Preferences) does not need
        // a renderer re-init -- dk2ShadowsEnabled() gates usage, not setup.
        id<MTLFunction> shadowMaskVertex = [library newFunctionWithName:@"dk2_shadow_mask_vertex"];
        id<MTLFunction> shadowMaskFragment = [library newFunctionWithName:@"dk2_shadow_mask_fragment"];
        MTLRenderPipelineDescriptor *shadowPipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
        shadowPipelineDescriptor.label = @"DK2 shadow mask";
        shadowPipelineDescriptor.vertexFunction = shadowMaskVertex;
        shadowPipelineDescriptor.fragmentFunction = shadowMaskFragment;
        shadowPipelineDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
        shadowPipelineDescriptor.colorAttachments[0].blendingEnabled = YES;
        shadowPipelineDescriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        shadowPipelineDescriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
        shadowPipelineDescriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        shadowPipelineDescriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        shadowPipelineDescriptor.rasterSampleCount = 1;
        _shadowMaskPipeline = shadowMaskVertex && shadowMaskFragment
            ? [_device newRenderPipelineStateWithDescriptor:shadowPipelineDescriptor error:&error] : nil;

        id<MTLFunction> shadowResolveVertex = [library newFunctionWithName:@"dk2_shadow_resolve_vertex"];
        id<MTLFunction> shadowResolveFragment = [library newFunctionWithName:@"dk2_shadow_resolve_fragment"];
        shadowPipelineDescriptor.label = @"DK2 shadow resolve";
        shadowPipelineDescriptor.vertexFunction = shadowResolveVertex;
        shadowPipelineDescriptor.fragmentFunction = shadowResolveFragment;
        shadowPipelineDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        shadowPipelineDescriptor.colorAttachments[0].blendingEnabled = NO;
        _shadowResolvePipeline = shadowResolveVertex && shadowResolveFragment
            ? [_device newRenderPipelineStateWithDescriptor:shadowPipelineDescriptor error:&error] : nil;
        _shadowsAvailable = _shadowMaskPipeline && _shadowResolvePipeline;
        if (!_shadowsAvailable) {
            // Cosmetic extra, exactly like bloom: log loudly and keep the
            // renderer alive with shadows off rather than failing startup.
            NSLog(@"WARNING: Metal shadow pipeline setup failed (%@); continuing with shadows disabled.",
                  error.localizedDescription ?: @"shadow function(s) missing from DK2Shaders.metallib");
        } else {
            NSLog(@"DK2 shadows: original projection GPU rasterizer ready, currently %s "
                  "(scratch=%lux%lu, masks=%lu); settings.toml [renderer] metal_shadows, "
                  "or DK2_METAL_SHADOWS=0/1.",
                  dk2ShadowsEnabled() ? "enabled" : "disabled",
                  (unsigned long)kShadowScratchSize, (unsigned long)kShadowScratchSize,
                  (unsigned long)kMaxShadowMasksPerFrame);
        }
    }
    // Creates the user-replaceable pack directory on first launch and logs
    // where to drop named PNGs (see the respack namespace).
    NSLog(@"DK2 resource pack: named HD textures read from %@", respack::candidatePath());
    _hdTexturesLive = dk2cfg::gHdTexturesLive.load(std::memory_order_relaxed);

    for (uint32_t function = 1; function <= 8; ++function) {
        for (uint32_t write = 0; write <= 1; ++write) {
            MTLDepthStencilDescriptor *depthDescriptor = [[MTLDepthStencilDescriptor alloc] init];
            depthDescriptor.label = [NSString stringWithFormat:@"DK2 depth %u/%u", function, write];
            depthDescriptor.depthCompareFunction = metalCompareFunction(function);
            depthDescriptor.depthWriteEnabled = write != 0;
            _depthStates[function][write] = [_device newDepthStencilStateWithDescriptor:depthDescriptor];
        }
    }

    MTLSamplerDescriptor *samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
    samplerDescriptor.label = @"DK2 fixed-function sampler";
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeRepeat;
    samplerDescriptor.tAddressMode = MTLSamplerAddressModeRepeat;
    // HD textures carry mip chains; originals have a single level, for which
    // the mip filter and anisotropy are inert
    samplerDescriptor.mipFilter = MTLSamplerMipFilterLinear;
    samplerDescriptor.maxAnisotropy = 8;
    samplerDescriptor.supportArgumentBuffers = YES;
    _sampler = [_device newSamplerStateWithDescriptor:samplerDescriptor];

    {
        // Always created regardless of the current bloom setting (see the
        // shadow pipeline above for why): dk2BloomEnabled() gates usage.
        // Clamp (not repeat) so the fullscreen bloom passes never sample
        // across the opposite screen edge near the border.
        MTLSamplerDescriptor *bloomSamplerDescriptor = [[MTLSamplerDescriptor alloc] init];
        bloomSamplerDescriptor.label = @"DK2 bloom sampler";
        bloomSamplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
        bloomSamplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
        bloomSamplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
        bloomSamplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
        bloomSamplerDescriptor.supportArgumentBuffers = YES;
        _bloomSampler = [_device newSamplerStateWithDescriptor:bloomSamplerDescriptor];

        id<MTLFunction> bloomVertexFunction = [library newFunctionWithName:@"dk2_bloom_vertex"];
        id<MTLFunction> bloomThresholdFragment = [library newFunctionWithName:@"dk2_bloom_threshold"];
        id<MTLFunction> bloomBlurHorizontalFragment = [library newFunctionWithName:@"dk2_bloom_blur_horizontal"];
        id<MTLFunction> bloomBlurVerticalFragment = [library newFunctionWithName:@"dk2_bloom_blur_vertical"];
        id<MTLFunction> bloomCompositeFragment = [library newFunctionWithName:@"dk2_bloom_composite"];

        MTLRenderPipelineDescriptor *bloomPipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
        bloomPipelineDescriptor.label = @"DK2 bloom threshold";
        bloomPipelineDescriptor.vertexFunction = bloomVertexFunction;
        bloomPipelineDescriptor.fragmentFunction = bloomThresholdFragment;
        bloomPipelineDescriptor.colorAttachments[0].pixelFormat = _layer.pixelFormat;
        bloomPipelineDescriptor.colorAttachments[0].blendingEnabled = NO;
        _bloomThresholdPipeline = bloomVertexFunction && bloomThresholdFragment
            ? [_device newRenderPipelineStateWithDescriptor:bloomPipelineDescriptor error:&error] : nil;

        bloomPipelineDescriptor.label = @"DK2 bloom blur horizontal";
        bloomPipelineDescriptor.fragmentFunction = bloomBlurHorizontalFragment;
        _bloomBlurHorizontalPipeline = bloomVertexFunction && bloomBlurHorizontalFragment
            ? [_device newRenderPipelineStateWithDescriptor:bloomPipelineDescriptor error:&error] : nil;

        bloomPipelineDescriptor.label = @"DK2 bloom blur vertical";
        bloomPipelineDescriptor.fragmentFunction = bloomBlurVerticalFragment;
        _bloomBlurVerticalPipeline = bloomVertexFunction && bloomBlurVerticalFragment
            ? [_device newRenderPipelineStateWithDescriptor:bloomPipelineDescriptor error:&error] : nil;

        bloomPipelineDescriptor.label = @"DK2 bloom composite";
        bloomPipelineDescriptor.fragmentFunction = bloomCompositeFragment;
        _bloomCompositePipeline = bloomVertexFunction && bloomCompositeFragment
            ? [_device newRenderPipelineStateWithDescriptor:bloomPipelineDescriptor error:&error] : nil;

        // Bloom is a cosmetic extra layered on top of a renderer that must
        // otherwise come up: a metallib mismatch or pipeline-state failure
        // here logs loudly and disables bloom for the process lifetime
        // instead of taking down the whole renderer via fail()/return nil.
        _bloomAvailable = _bloomSampler && _bloomThresholdPipeline && _bloomBlurHorizontalPipeline &&
            _bloomBlurVerticalPipeline && _bloomCompositePipeline;
        if (!_bloomAvailable) {
            NSLog(@"WARNING: Metal bloom setup failed (%@); continuing with bloom disabled.",
                  error.localizedDescription ?: @"bloom function(s) missing from DK2Shaders.metallib");
            _bloomSampler = nil;
            _bloomThresholdPipeline = nil;
            _bloomBlurHorizontalPipeline = nil;
            _bloomBlurVerticalPipeline = nil;
            _bloomCompositePipeline = nil;
        } else {
            // Threshold/intensity are literal constants in DK2Shaders.metal
            // (kDK2BloomThreshold/kDK2BloomIntensity) - kept in sync here as
            // a comment only, this is just a startup confirmation log.
            NSLog(@"DK2 bloom: pipeline ready, currently %s (threshold=0.65 intensity=0.35); "
                  "settings.toml [renderer] bloom, or DK2_BLOOM=0/1.",
                  dk2BloomEnabled() ? "enabled" : "disabled");
        }
    }

    MTLTextureDescriptor *whiteDescriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                    width:1 height:1 mipmapped:NO];
    whiteDescriptor.storageMode = MTLStorageModeShared;
    whiteDescriptor.usage = MTLTextureUsageShaderRead;
    _whiteTexture = [_device newTextureWithDescriptor:whiteDescriptor];
    const uint32_t whitePixel = 0xFFFFFFFFu;
    [_whiteTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                     mipmapLevel:0 withBytes:&whitePixel bytesPerRow:4];
    _commandViews.reserve(2048);
    _frameTextureBindings.reserve(kTextureBindingsPerArgumentTable *
                                  kTextureArgumentTablesPerFrame);
    if (!_depthStates[8][0] || !_depthStates[4][1] || !_sampler || !_whiteTexture) {
        fail(@"Metal fixed-function resource creation failed.");
        return nil;
    }

    MTLResidencySetDescriptor *residencyDescriptor = [[MTLResidencySetDescriptor alloc] init];
    residencyDescriptor.initialCapacity = 2048;
    residencyDescriptor.label = @"DK2 dynamic frame resources";
    _resources = [_device newResidencySetWithDescriptor:residencyDescriptor error:&error];
    if (!_resources) {
        fail([NSString stringWithFormat:@"Metal residency set failed: %@", error.localizedDescription]);
        return nil;
    }

    for (NSUInteger index = 0; index < kFramesInFlight; ++index) {
        _allocators[index] = [_device newCommandAllocator];
        _commandBuffers[index] = [_device newCommandBuffer];
        _commandBuffers[index].label = [NSString stringWithFormat:@"DK2 frame %lu", index];
        if (!_allocators[index] || !_commandBuffers[index]) {
            fail(@"Metal 4 frame resource creation failed.");
            return nil;
        }
        const MTLResourceOptions uploadOptions = MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
        _vertexBuffers[index] = [_device newBufferWithLength:kVertexBufferSize options:uploadOptions];
        _indexBuffers[index] = [_device newBufferWithLength:kIndexBufferSize options:uploadOptions];
        _drawBuffers[index] = [_device newBufferWithLength:kDrawBufferSize options:uploadOptions];
        _vertexBuffers[index].label = [NSString stringWithFormat:@"DK2 vertices %lu", index];
        _indexBuffers[index].label = [NSString stringWithFormat:@"DK2 indices %lu", index];
        _drawBuffers[index].label = [NSString stringWithFormat:@"DK2 draw uniforms %lu", index];

        _meshVertexBuffers[index] = [_device newBufferWithLength:kMeshVertexBufferSize options:uploadOptions];
        _meshDrawBuffers[index] = [_device newBufferWithLength:kMeshDrawBufferSize options:uploadOptions];
        _lightsBuffers[index] = [_device newBufferWithLength:kLightsBufferSize options:uploadOptions];
        _cameraBuffers[index] = [_device newBufferWithLength:kCameraBufferSize options:uploadOptions];
        _meshVertexBuffers[index].label = [NSString stringWithFormat:@"DK2 mesh vertices %lu", index];
        _meshDrawBuffers[index].label = [NSString stringWithFormat:@"DK2 mesh draws %lu", index];
        _lightsBuffers[index].label = [NSString stringWithFormat:@"DK2 lights %lu", index];
        _cameraBuffers[index].label = [NSString stringWithFormat:@"DK2 camera %lu", index];
        if (!_vertexBuffers[index] || !_indexBuffers[index] || !_drawBuffers[index] ||
            !_meshVertexBuffers[index] || !_meshDrawBuffers[index] ||
            !_lightsBuffers[index] || !_cameraBuffers[index]) {
            fail(@"Metal dynamic frame buffer creation failed.");
            return nil;
        }

        if (_shadowsAvailable) {
            _shadowVertexBuffers[index] =
                [_device newBufferWithLength:kShadowVertexBufferSize options:uploadOptions];
            _shadowResolveBuffers[index] =
                [_device newBufferWithLength:kShadowResolveBufferSize options:uploadOptions];
            MTLTextureDescriptor *shadowDescriptor = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                            width:kShadowScratchSize
                                           height:kShadowScratchSize
                                        mipmapped:NO];
            shadowDescriptor.storageMode = MTLStorageModePrivate;
            shadowDescriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            _shadowScratchTextures[index] = [_device newTextureWithDescriptor:shadowDescriptor];
            _shadowVertexBuffers[index].label = [NSString stringWithFormat:@"DK2 shadow vertices %lu", index];
            _shadowResolveBuffers[index].label = [NSString stringWithFormat:@"DK2 shadow resolves %lu", index];
            _shadowScratchTextures[index].label = [NSString stringWithFormat:@"DK2 shadow scratch %lu", index];
            if (!_shadowVertexBuffers[index] || !_shadowResolveBuffers[index] ||
                !_shadowScratchTextures[index]) {
                NSLog(@"WARNING: Metal shadow resource allocation failed for frame slot %lu; "
                       "disabling shadows.", index);
                _shadowsAvailable = NO;
                _shadowVertexBuffers[index] = nil;
                _shadowResolveBuffers[index] = nil;
                _shadowScratchTextures[index] = nil;
            } else {
                MTL4ArgumentTableDescriptor *maskTableDescriptor =
                    [[MTL4ArgumentTableDescriptor alloc] init];
                maskTableDescriptor.maxBufferBindCount = 1;
                maskTableDescriptor.initializeBindings = YES;
                maskTableDescriptor.label = [NSString stringWithFormat:@"DK2 shadow mask arguments %lu", index];
                _shadowMaskArgumentTables[index] =
                    [_device newArgumentTableWithDescriptor:maskTableDescriptor error:&error];

                MTL4ArgumentTableDescriptor *resolveTableDescriptor =
                    [[MTL4ArgumentTableDescriptor alloc] init];
                resolveTableDescriptor.maxBufferBindCount = 1;
                resolveTableDescriptor.maxTextureBindCount = 1;
                resolveTableDescriptor.initializeBindings = YES;
                resolveTableDescriptor.label = [NSString stringWithFormat:@"DK2 shadow resolve arguments %lu", index];
                _shadowResolveArgumentTables[index] =
                    [_device newArgumentTableWithDescriptor:resolveTableDescriptor error:&error];
                if (!_shadowMaskArgumentTables[index] || !_shadowResolveArgumentTables[index]) {
                    NSLog(@"WARNING: Metal shadow argument table creation failed for frame slot %lu; "
                           "disabling shadows.", index);
                    _shadowsAvailable = NO;
                }
            }
        }
        for (NSUInteger bank = 0; bank < kTextureArgumentTablesPerFrame; ++bank) {
            MTL4ArgumentTableDescriptor *tableDescriptor =
                [[MTL4ArgumentTableDescriptor alloc] init];
            tableDescriptor.maxBufferBindCount = 7;
            tableDescriptor.maxTextureBindCount = kTextureBindingsPerArgumentTable;
            tableDescriptor.maxSamplerStateBindCount = 1;
            tableDescriptor.initializeBindings = YES;
            tableDescriptor.label = [NSString
                stringWithFormat:@"DK2 arguments %lu/%lu", index, bank];
            _argumentTables[index][bank] =
                [_device newArgumentTableWithDescriptor:tableDescriptor error:&error];
            if (!_argumentTables[index][bank]) {
                fail(@"Metal dynamic argument table creation failed.");
                return nil;
            }
            [_argumentTables[index][bank]
                setTexture:_whiteTexture.gpuResourceID atIndex:0];
            [_argumentTables[index][bank]
                setSamplerState:_sampler.gpuResourceID atIndex:0];
        }
        if (_bloomAvailable) {
            MTL4ArgumentTableDescriptor *bloomTableDescriptor =
                [[MTL4ArgumentTableDescriptor alloc] init];
            bloomTableDescriptor.maxTextureBindCount = 2;
            bloomTableDescriptor.maxSamplerStateBindCount = 1;
            bloomTableDescriptor.initializeBindings = YES;
            bloomTableDescriptor.label = [NSString stringWithFormat:@"DK2 bloom arguments %lu", index];
            _bloomArgumentTables[index] =
                [_device newArgumentTableWithDescriptor:bloomTableDescriptor error:&error];
            if (!_bloomArgumentTables[index]) {
                // Same cosmetic-extra reasoning as the pipeline setup above:
                // log loudly, disable bloom for good, keep the renderer alive.
                NSLog(@"WARNING: Metal bloom argument table creation failed for frame slot %lu (%@); "
                       "disabling bloom.", index, error.localizedDescription ?: @"unknown error");
                _bloomAvailable = NO;
                _bloomSampler = nil;
                _bloomThresholdPipeline = nil;
                _bloomBlurHorizontalPipeline = nil;
                _bloomBlurVerticalPipeline = nil;
                _bloomCompositePipeline = nil;
                continue;
            }
            [_bloomArgumentTables[index] setSamplerState:_bloomSampler.gpuResourceID atIndex:0];
        }
    }

    id<MTLAllocation> allocations[kFramesInFlight * 7 + 1];
    for (NSUInteger index = 0; index < kFramesInFlight; ++index) {
        allocations[index * 7] = _vertexBuffers[index];
        allocations[index * 7 + 1] = _indexBuffers[index];
        allocations[index * 7 + 2] = _drawBuffers[index];
        allocations[index * 7 + 3] = _meshVertexBuffers[index];
        allocations[index * 7 + 4] = _meshDrawBuffers[index];
        allocations[index * 7 + 5] = _lightsBuffers[index];
        allocations[index * 7 + 6] = _cameraBuffers[index];
    }
    allocations[kFramesInFlight * 7] = _whiteTexture;
    [_resources addAllocations:allocations count:kFramesInFlight * 7 + 1];
    if (_shadowsAvailable) {
        id<MTLAllocation> shadowAllocations[kFramesInFlight * 3];
        for (NSUInteger index = 0; index < kFramesInFlight; ++index) {
            shadowAllocations[index * 3] = _shadowVertexBuffers[index];
            shadowAllocations[index * 3 + 1] = _shadowResolveBuffers[index];
            shadowAllocations[index * 3 + 2] = _shadowScratchTextures[index];
        }
        [_resources addAllocations:shadowAllocations count:kFramesInFlight * 3];
    }
    [_resources commit];
    [_resources requestResidency];
    [_queue addResidencySet:_resources];
    gShadowGpuAvailable.store(_shadowsAvailable, std::memory_order_release);

    if (gBridgePath) {
        _bridge = std::make_unique<BridgeReader>(gBridgePath.fileSystemRepresentation);
        if (!_bridge->valid()) {
            fail([NSString stringWithFormat:@"Unable to map bridge file: %@", gBridgePath]);
            return nil;
        }
    }

    _displayLink = [[CAMetalDisplayLink alloc] initWithMetalLayer:_layer];
    _displayLink.delegate = self;
    _displayLink.preferredFrameRateRange = CAFrameRateRangeMake(30.0, 120.0, 120.0);
    _displayLink.preferredFrameLatency = 2.0;
    return self;
}

- (BOOL)ensureRenderTargetsWidth:(NSUInteger)width height:(NSUInteger)height {
    if (_multisampleColorTexture && _depthTexture &&
        _multisampleColorTexture.width == width && _multisampleColorTexture.height == height) return YES;
    if (_multisampleColorTexture || _depthTexture) {
        if (_frame && ![_completed waitUntilSignaledValue:_frame timeoutMS:1000]) return NO;
        // _multisampleColorTexture/_depthTexture are never added to
        // _resources (see below - they are MTLStorageModeMemoryless, which
        // has no IOAccelResource/GPU memory backing to make resident), so
        // nothing to remove here for them.
        if (_sceneColorTexture) [_resources removeAllocation:_sceneColorTexture];
        if (_bloomTextureA) [_resources removeAllocation:_bloomTextureA];
        if (_bloomTextureB) [_resources removeAllocation:_bloomTextureB];
    }
    // Both attachments are write-then-consume-within-the-same-pass only: the
    // MSAA color is always resolved (MTLStoreActionMultisampleResolve, never
    // stored itself) and the depth buffer is always MTLStoreActionDontCare
    // (never sampled - see the grep-checked absence of any shader read of
    // either texture). Neither is ever read back after the pass ends, so
    // both are safe as MTLStorageModeMemoryless (TBDR on-chip tile memory
    // only, no GPU memory backing) instead of MTLStorageModePrivate.
    MTLTextureDescriptor *colorDescriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:_layer.pixelFormat
                                    width:width height:height mipmapped:NO];
    colorDescriptor.textureType = MTLTextureType2DMultisample;
    colorDescriptor.sampleCount = kSampleCount;
    colorDescriptor.storageMode = MTLStorageModeMemoryless;
    colorDescriptor.usage = MTLTextureUsageRenderTarget;
    _multisampleColorTexture = [_device newTextureWithDescriptor:colorDescriptor];

    MTLTextureDescriptor *depthDescriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                    width:width height:height mipmapped:NO];
    depthDescriptor.textureType = MTLTextureType2DMultisample;
    depthDescriptor.sampleCount = kSampleCount;
    depthDescriptor.storageMode = MTLStorageModeMemoryless;
    depthDescriptor.usage = MTLTextureUsageRenderTarget;
    _depthTexture = [_device newTextureWithDescriptor:depthDescriptor];
    if (!_multisampleColorTexture || !_depthTexture) return NO;
    _multisampleColorTexture.label = @"DK2 4x MSAA color";
    _depthTexture.label = @"DK2 4x MSAA depth";
    // Memoryless textures have no GPU memory backing, so they are never
    // added to _resources (an MTLResidencySet manages residency of actual
    // memory allocations - there is nothing here for it to make resident).

    // Allocated whenever the pipeline exists, independent of the current
    // bloom setting, so toggling bloom on live (without a resize) has a
    // target ready: dk2BloomEnabled() gates only per-frame usage below.
    if (_bloomAvailable) {
        MTLTextureDescriptor *sceneDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:_layer.pixelFormat
                                        width:width height:height mipmapped:NO];
        sceneDescriptor.storageMode = MTLStorageModePrivate;
        sceneDescriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        _sceneColorTexture = [_device newTextureWithDescriptor:sceneDescriptor];
        _sceneColorTexture.label = @"DK2 bloom scene color";

        const NSUInteger bloomWidth = std::max<NSUInteger>(1, width / 2);
        const NSUInteger bloomHeight = std::max<NSUInteger>(1, height / 2);
        MTLTextureDescriptor *bloomDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:_layer.pixelFormat
                                        width:bloomWidth height:bloomHeight mipmapped:NO];
        bloomDescriptor.storageMode = MTLStorageModePrivate;
        bloomDescriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        _bloomTextureA = [_device newTextureWithDescriptor:bloomDescriptor];
        _bloomTextureB = [_device newTextureWithDescriptor:bloomDescriptor];
        if (!_sceneColorTexture || !_bloomTextureA || !_bloomTextureB) {
            // Never fail the base scene targets over a cosmetic extra: log
            // loudly, drop back to presenting straight to the drawable (the
            // per-frame bloomActive check below re-derives from these
            // pointers, so nil here is enough to disable it), and keep going.
            NSLog(@"WARNING: Metal bloom render target allocation failed at %lux%lu; "
                   "disabling bloom.", (unsigned long)width, (unsigned long)height);
            _bloomAvailable = NO;
            _sceneColorTexture = nil;
            _bloomTextureA = nil;
            _bloomTextureB = nil;
            return YES;
        }
        _bloomTextureA.label = @"DK2 bloom half-res A";
        _bloomTextureB.label = @"DK2 bloom half-res B";
        id<MTLAllocation> bloomTargets[] = {_sceneColorTexture, _bloomTextureA, _bloomTextureB};
        [_resources addAllocations:bloomTargets count:3];
    }

    [_resources commit];
    return YES;
}

// Threshold -> half-res separable blur -> additive composite onto the
// drawable. Runs entirely as small fullscreen-triangle render passes so it
// slots into the existing MTL4 render-command-encoder flow without a compute
// pipeline. Only called when dk2BloomEnabled() and the bloom targets exist.
- (void)encodeBloomIntoCommandBuffer:(id<MTL4CommandBuffer>)commandBuffer
                                 slot:(NSUInteger)slot
                     drawableTexture:(id<MTLTexture>)drawableTexture {
    id<MTL4ArgumentTable> table = _bloomArgumentTables[slot];
    // Defense in depth: the caller only invokes this when every one of these
    // is already known non-nil, but never risk encoding a pass with a nil
    // pipeline/table/texture (that is API-validation-fatal, unlike an
    // Objective-C nil message send) - log once and bail so the frame still
    // presents (unresolved bloom this frame, not a torn-down process).
    if (!table || !_bloomThresholdPipeline || !_bloomBlurHorizontalPipeline ||
        !_bloomBlurVerticalPipeline || !_bloomCompositePipeline ||
        !_sceneColorTexture || !_bloomTextureA || !_bloomTextureB) {
        static bool loggedOnce = false;
        if (!loggedOnce) {
            loggedOnce = true;
            NSLog(@"WARNING: encodeBloomIntoCommandBuffer called with a missing bloom "
                   "resource; skipping bloom for this and later frames.");
        }
        _bloomAvailable = NO;
        return;
    }

    [table setTexture:_sceneColorTexture.gpuResourceID atIndex:0];
    MTL4RenderPassDescriptor *thresholdPass = [[MTL4RenderPassDescriptor alloc] init];
    thresholdPass.colorAttachments[0].texture = _bloomTextureA;
    thresholdPass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    thresholdPass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> thresholdEncoder =
        [commandBuffer renderCommandEncoderWithDescriptor:thresholdPass];
    thresholdEncoder.label = @"DK2 bloom threshold";
    // Metal 4 resources are untracked: wait for the main scene pass's MSAA
    // resolve into _sceneColorTexture (sampled here at index 0) before this
    // pass's fragment stage reads it. Fragment->Fragment is coarser than the
    // resolve itself needs; narrow once a GPU trace confirms the real
    // producer stage.
    [thresholdEncoder barrierAfterQueueStages:MTLStageFragment
                                  beforeStages:MTLStageFragment
                             visibilityOptions:MTL4VisibilityOptionDevice];
    [thresholdEncoder setArgumentTable:table atStages:MTLRenderStageFragment];
    [thresholdEncoder setRenderPipelineState:_bloomThresholdPipeline];
    [thresholdEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [thresholdEncoder endEncoding];

    [table setTexture:_bloomTextureA.gpuResourceID atIndex:0];
    MTL4RenderPassDescriptor *blurHorizontalPass = [[MTL4RenderPassDescriptor alloc] init];
    blurHorizontalPass.colorAttachments[0].texture = _bloomTextureB;
    blurHorizontalPass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    blurHorizontalPass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> blurHorizontalEncoder =
        [commandBuffer renderCommandEncoderWithDescriptor:blurHorizontalPass];
    blurHorizontalEncoder.label = @"DK2 bloom blur horizontal";
    // Metal 4 resources are untracked: wait for the threshold pass's write
    // to _bloomTextureA (sampled here at index 0) before this pass's
    // fragment stage reads it. See the threshold barrier above for the
    // narrowing note.
    [blurHorizontalEncoder barrierAfterQueueStages:MTLStageFragment
                                       beforeStages:MTLStageFragment
                                  visibilityOptions:MTL4VisibilityOptionDevice];
    [blurHorizontalEncoder setArgumentTable:table atStages:MTLRenderStageFragment];
    [blurHorizontalEncoder setRenderPipelineState:_bloomBlurHorizontalPipeline];
    [blurHorizontalEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [blurHorizontalEncoder endEncoding];

    [table setTexture:_bloomTextureB.gpuResourceID atIndex:0];
    MTL4RenderPassDescriptor *blurVerticalPass = [[MTL4RenderPassDescriptor alloc] init];
    blurVerticalPass.colorAttachments[0].texture = _bloomTextureA;
    blurVerticalPass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    blurVerticalPass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> blurVerticalEncoder =
        [commandBuffer renderCommandEncoderWithDescriptor:blurVerticalPass];
    blurVerticalEncoder.label = @"DK2 bloom blur vertical";
    // Metal 4 resources are untracked: wait for the horizontal blur pass's
    // write to _bloomTextureB (sampled here at index 0) before this pass's
    // fragment stage reads it. See the threshold barrier above for the
    // narrowing note.
    [blurVerticalEncoder barrierAfterQueueStages:MTLStageFragment
                                     beforeStages:MTLStageFragment
                                visibilityOptions:MTL4VisibilityOptionDevice];
    [blurVerticalEncoder setArgumentTable:table atStages:MTLRenderStageFragment];
    [blurVerticalEncoder setRenderPipelineState:_bloomBlurVerticalPipeline];
    [blurVerticalEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [blurVerticalEncoder endEncoding];

    [table setTexture:_sceneColorTexture.gpuResourceID atIndex:0];
    [table setTexture:_bloomTextureA.gpuResourceID atIndex:1];
    MTL4RenderPassDescriptor *compositePass = [[MTL4RenderPassDescriptor alloc] init];
    compositePass.colorAttachments[0].texture = drawableTexture;
    compositePass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    compositePass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> compositeEncoder =
        [commandBuffer renderCommandEncoderWithDescriptor:compositePass];
    compositeEncoder.label = @"DK2 bloom composite";
    // Metal 4 resources are untracked: wait for the vertical blur pass's
    // write to _bloomTextureA (sampled here at index 1, alongside
    // _sceneColorTexture at index 0 which the main scene pass's own barrier
    // already covered) before this pass's fragment stage reads it. See the
    // threshold barrier above for the narrowing note.
    [compositeEncoder barrierAfterQueueStages:MTLStageFragment
                                  beforeStages:MTLStageFragment
                             visibilityOptions:MTL4VisibilityOptionDevice];
    [compositeEncoder setArgumentTable:table atStages:MTLRenderStageFragment];
    [compositeEncoder setRenderPipelineState:_bloomCompositePipeline];
    [compositeEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [compositeEncoder endEncoding];
}

static void *renderWorker(void *context) {
    @autoreleasepool {
        pthread_setname_np("DK2 Metal Render");
        DK2MetalRenderer *renderer = (__bridge DK2MetalRenderer *)context;
        [renderer->_displayLink addToRunLoop:NSRunLoop.currentRunLoop forMode:NSDefaultRunLoopMode];
        [NSRunLoop.currentRunLoop run];
    }
    return nullptr;
}

- (void)start {
    pthread_attr_t attributes;
    int result = pthread_attr_init(&attributes);
    NSAssert(result == 0, @"Unable to initialize render thread attributes");
    result = pthread_attr_setschedpolicy(&attributes, SCHED_RR);
    NSAssert(result == 0, @"Unable to set render thread scheduling policy");
    sched_param parameters = {.sched_priority = 45};
    result = pthread_attr_setschedparam(&attributes, &parameters);
    NSAssert(result == 0, @"Unable to set render thread priority");
    result = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    NSAssert(result == 0, @"Unable to detach render thread");

    pthread_t thread;
    result = pthread_create(&thread, &attributes, renderWorker, (__bridge void *)self);
    pthread_attr_destroy(&attributes);
    NSAssert(result == 0, @"Unable to create render thread");
}

- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update {
    @autoreleasepool {
        const auto encodeStarted = TelemetryClock::now();
        FrameMetrics metrics = {};
        __block BOOL residencyChanged = NO;
        const uint64_t completedValue = _completed.signaledValue;
        for (auto retired = _retiredAllocations.begin();
             retired != _retiredAllocations.end();) {
            if (retired->afterValue > completedValue) {
                ++retired;
                continue;
            }
            [_resources removeAllocation:retired->allocation];
            retired = _retiredAllocations.erase(retired);
            residencyChanged = YES;
        }
        for (NSUInteger index = 0; index < kFramesInFlight; ++index) {
            if (_submittedValue[index] && _submittedValue[index] <= completedValue) {
                metrics.gpuCompleteUs = std::max(
                    metrics.gpuCompleteUs,
                    elapsedMicroseconds(_submittedAt[index], encodeStarted));
                _submittedValue[index] = 0;
            }
        }
        const FrameSnapshot *snapshot = _bridge ? _bridge->poll() : nullptr;
        const BOOL hdTexturesLive =
            dk2cfg::gHdTexturesLive.load(std::memory_order_relaxed) ? YES : NO;
        if (hdTexturesLive != _hdTexturesLive) {
            _hdTexturesLive = hdTexturesLive;
            respack::liveStateChanged();
            NSLog(@"DK2 resource pack: live %@; %zu mapped pages scheduled for refresh",
                  hdTexturesLive ? @"on" : @"off", respack::pages().size());
        }
        const bool newBridgeFrame = snapshot && snapshot->frame != _lastBridgeFrame;
        if (gSelfTestFrames == 0 && _bridge && snapshot && snapshot->frame == _lastBridgeFrame) return;

        if (newBridgeFrame) {
            metrics.intervalUs = _lastBridgeArrival.time_since_epoch().count()
                                     ? elapsedMicroseconds(_lastBridgeArrival, encodeStarted) : 0;
            _lastBridgeArrival = encodeStarted;
            metrics.sceneUs = snapshot->sceneMicroseconds;
            metrics.tickUs = snapshot->tickMicroseconds;
            metrics.prepareUs = snapshot->prepareMicroseconds;
            metrics.drawUs = snapshot->drawMicroseconds;
            metrics.producerDrawCopyUs = snapshot->producerDrawCopyMicroseconds;
            metrics.producerTextureUs = snapshot->producerTextureMicroseconds;
            metrics.producerOverlayUs = snapshot->producerOverlayMicroseconds;
            metrics.bridgeBytes = static_cast<uint32_t>(snapshot->bytes.size());
            metrics.commands = snapshot->commandCount;
            _commandViews.clear();
            size_t commandOffset = 0;
            while (commandOffset + sizeof(DK2MCommandHeader) <= snapshot->bytes.size()) {
                DK2MCommandHeader header;
                std::memcpy(&header, snapshot->bytes.data() + commandOffset, sizeof(header));
                if (header.size < sizeof(header) ||
                    commandOffset + header.size > snapshot->bytes.size()) break;
                _commandViews.push_back({header.type, static_cast<uint32_t>(commandOffset),
                                         header.size});
                commandOffset += header.size;
            }
        }

        // PAGE_ATLAS_MAP is persistent metadata, not a draw command. Read all
        // maps before processing uploads so map+upload works regardless of
        // their order in a captured frame.
        if (snapshot) {
            for (const CommandView &view : _commandViews) {
                if (view.type != DK2M_COMMAND_PAGE_ATLAS_MAP) continue;
                ++metrics.packMapCommands;
                DK2MPageAtlasMapCommand map = {};
                if (decodeAtlasMap(snapshot, view, map) && respack::noteRect(map)) {
                    ++metrics.packMappedRects;
                }
            }
        }

        const NSUInteger slot = _frame % kFramesInFlight;
        if (_frame >= kFramesInFlight) {
            const uint64_t required = _frame - kFramesInFlight + 1;
            const auto waitStarted = TelemetryClock::now();
            if (![_completed waitUntilSignaledValue:required timeoutMS:1000]) {
                fail(@"GPU frame completion timed out.");
                link.paused = YES;
                return;
            }
            const auto waitFinished = TelemetryClock::now();
            metrics.gpuWaitUs = elapsedMicroseconds(waitStarted, waitFinished);
            if (_submittedValue[slot]) {
                metrics.gpuCompleteUs = std::max(
                    metrics.gpuCompleteUs,
                    elapsedMicroseconds(_submittedAt[slot], waitFinished));
                _submittedValue[slot] = 0;
            }
        }
        if (![self ensureRenderTargetsWidth:update.drawable.texture.width
                                        height:update.drawable.texture.height]) {
            fail(@"Metal multisample target creation failed.");
            link.paused = YES;
            return;
        }

        [_allocators[slot] reset];
        id<MTL4CommandBuffer> commandBuffer = _commandBuffers[slot];
        [commandBuffer beginCommandBufferWithAllocator:_allocators[slot]];

        const double t = update.targetPresentationTimestamp;
        const double pulse = 0.5 + 0.5 * std::sin(t * 1.8);
        MTLClearColor clearColor = MTLClearColorMake(0.025 + pulse * 0.035, 0.07, 0.10 + pulse * 0.08, 1.0);
        void (^retireTexture)(id<MTLTexture>) = ^(id<MTLTexture> texture) {
            if (!texture) return;
            const auto duplicate = std::find_if(
                _retiredAllocations.begin(), _retiredAllocations.end(),
                [texture](const RetiredAllocation &entry) {
                    return entry.allocation == texture;
                });
            if (duplicate == _retiredAllocations.end()) {
                _retiredAllocations.push_back({texture, _frame});
            }
        };
        void (^installTexture)(uint32_t, id<MTLTexture>) =
            ^(uint32_t textureId, id<MTLTexture> texture) {
                if (!texture || _textures[textureId] == texture) return;
                retireTexture(_textures[textureId]);
                _textures[textureId] = texture;
                [_resources addAllocation:texture];
                residencyChanged = YES;
            };
        std::unordered_set<uint32_t> shadowTargetIds;
        if (snapshot && _shadowsAvailable) {
            for (const CommandView &view : _commandViews) {
                if (view.type != DK2M_COMMAND_SHADOW_MASK ||
                    view.size < sizeof(DK2MShadowMaskCommand)) continue;
                DK2MShadowMaskCommand shadow{};
                std::memcpy(&shadow, snapshot->bytes.data() + view.offset, sizeof(shadow));
                const uint64_t expected = sizeof(shadow) +
                    static_cast<uint64_t>(shadow.triangle_count) * sizeof(DK2MShadowTriangle);
                if (shadow.texture_id && expected <= view.size) {
                    shadowTargetIds.insert(shadow.texture_id);
                }
            }
        }
        // A live off->on switch can produce a mask without changing the CPU
        // atlas bytes (the GPU scratch is deliberately blank). Promote an
        // already-resident shader-read atlas once so the resolve command is
        // never dependent on a coincidental full texture re-upload.
        for (uint32_t textureId : shadowTargetIds) {
            const auto found = _textures.find(textureId);
            id<MTLTexture> oldTexture = found == _textures.end() ? nil : found->second;
            if (!oldTexture || (oldTexture.usage & MTLTextureUsageRenderTarget) ||
                oldTexture.pixelFormat != MTLPixelFormatBGRA8Unorm ||
                oldTexture.storageMode == MTLStorageModePrivate) continue;
            DynamicTexture &dyn = _dynamicTextures[textureId];
            uint32_t targetWidth = dyn.sourceWidth;
            uint32_t targetHeight = dyn.sourceHeight;
            std::vector<uint8_t> pixels;
            if (!respack::originalPage(textureId, pixels, targetWidth, targetHeight)) {
                if (!targetWidth || !targetHeight) {
                    targetWidth = static_cast<uint32_t>(oldTexture.width);
                    targetHeight = static_cast<uint32_t>(oldTexture.height);
                }
                const NSUInteger oldRowBytes = oldTexture.width * 4;
                std::vector<uint8_t> oldPixels(oldRowBytes * oldTexture.height);
                [oldTexture getBytes:oldPixels.data() bytesPerRow:oldRowBytes
                          fromRegion:MTLRegionMake2D(0, 0, oldTexture.width, oldTexture.height)
                         mipmapLevel:0];
                pixels.resize(static_cast<size_t>(targetWidth) * targetHeight * 4);
                respack::scaleBilinearBgra(
                    oldPixels.data(), static_cast<uint32_t>(oldTexture.width),
                    static_cast<uint32_t>(oldTexture.height),
                    static_cast<uint32_t>(oldRowBytes), pixels.data(), targetWidth,
                    targetHeight, targetWidth * 4);
            }
            MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                            width:targetWidth
                                           height:targetHeight
                                        mipmapped:NO];
            descriptor.storageMode = MTLStorageModeShared;
            descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            id<MTLTexture> promoted = [_device newTextureWithDescriptor:descriptor];
            if (!promoted) continue;
            promoted.label = [NSString stringWithFormat:@"DK2 shadow atlas %u", textureId];
            [promoted replaceRegion:MTLRegionMake2D(0, 0, promoted.width, promoted.height)
                        mipmapLevel:0 withBytes:pixels.data() bytesPerRow:targetWidth * 4];
            for (auto &ringTexture : dyn.ring) {
                if (ringTexture != oldTexture) retireTexture(ringTexture);
                ringTexture = nil;
            }
            dyn.dynamic = true;
            dyn.ringIndex = 0;
            dyn.ring[0] = promoted;
            installTexture(textureId, promoted);
        }
        if (snapshot) {
            for (const CommandView &view : _commandViews) {
                const size_t offset = view.offset;
                if (view.type == DK2M_COMMAND_CLEAR && view.size == sizeof(DK2MClearCommand)) {
                    DK2MClearCommand clear;
                    std::memcpy(&clear, snapshot->bytes.data() + offset, sizeof(clear));
                    clearColor = MTLClearColorMake(clear.red, clear.green, clear.blue, clear.alpha);
                } else if (view.type == DK2M_COMMAND_TEXTURE_UPDATE &&
                           view.size >= sizeof(DK2MTextureUpdateCommand)) {
                    DK2MTextureUpdateCommand textureUpdate;
                    std::memcpy(&textureUpdate, snapshot->bytes.data() + offset, sizeof(textureUpdate));
                    const size_t expected = sizeof(textureUpdate) + textureUpdate.data_size;
                    const uint32_t key = textureUpdate.texture_id;
                    if (textureUpdate.texture_id && expected <= view.size &&
                        textureUpdate.width && textureUpdate.height &&
                        textureUpdate.row_pitch >= textureUpdate.width * 4 &&
                        textureUpdate.data_size >= textureUpdate.row_pitch * textureUpdate.height) {
                        ++metrics.textureUpdates;
                        metrics.textureBytes += textureUpdate.data_size;
                        // ponytail: periodic log of large per-frame texture
                        // uploads - ~8MB/frame is streaming through the bridge
                        // and this names the culprit id/size. Remove once the
                        // re-upload source is fixed.
                        if (textureUpdate.data_size >= 1024u * 1024u) {
                            static NSTimeInterval lastLoggedBigUpload = 0;
                            const NSTimeInterval now = CACurrentMediaTime();
                            if (now - lastLoggedBigUpload > 2.0) {
                                lastLoggedBigUpload = now;
                                NSLog(@"DIAG big texture upload: id=%u %ux%u bytes=%u",
                                      textureUpdate.texture_id, textureUpdate.width,
                                      textureUpdate.height, textureUpdate.data_size);
                            }
                        }
                        const uint8_t *pixels = snapshot->bytes.data() + offset + sizeof(textureUpdate);
                        DynamicTexture &dyn = _dynamicTextures[textureUpdate.texture_id];
                        dyn.sourceWidth = textureUpdate.width;
                        dyn.sourceHeight = textureUpdate.height;
                        id<MTLTexture> hd = nil;
                        const bool cursorTexture =
                            textureUpdate.texture_id == DK2M_CURSOR_TEXTURE_ID;
                        const bool shadowTarget =
                            shadowTargetIds.contains(textureUpdate.texture_id);
                        if (shadowTarget) {
                            dyn.dynamic = true;
                            respack::storeForShadow(textureUpdate.texture_id, pixels,
                                                   textureUpdate.width, textureUpdate.height,
                                                   textureUpdate.row_pitch);
                        }
                        // Dynamic ids periodically retry the HD paths so the
                        // classification can recover (it used to be permanent,
                        // which locked pages out of HD forever when the HD dir
                        // was empty/off while the misses accumulated).
                        if (!shadowTarget &&
                            (!dyn.dynamic || cursorTexture || (++dyn.retry & 127) == 0) &&
                            textureUpdate.texture_id != DK2M_OVERLAY_TEXTURE_ID) {
                            hd = respack::buildFromUpload(_device, key, pixels,
                                                          textureUpdate.width,
                                                          textureUpdate.height,
                                                          textureUpdate.row_pitch,
                                                          &metrics.packHits,
                                                          &metrics.packPagesComposed);
                            if (!hd) {
                                hd = texhd::lookup(_device, pixels, textureUpdate.width,
                                                   textureUpdate.height, textureUpdate.row_pitch);
                            }
                            if (hd) {
                                dyn.misses = 0;
                                if (dyn.dynamic) {
                                    id<MTLTexture> currentTexture = _textures[key];
                                    for (auto &ringTexture : dyn.ring) {
                                        if (ringTexture != currentTexture) retireTexture(ringTexture);
                                        ringTexture = nil;
                                    }
                                    dyn.ringIndex = 0;
                                    dyn.dynamic = false;
                                    NSLog(@"texture id %u recovered from dynamic: HD lookup back on",
                                          textureUpdate.texture_id);
                                }
                            } else if (!dyn.dynamic &&
                                       (texhd::directory() != nil || respack::directory() != nil) &&
                                       ++dyn.misses > 8 && !cursorTexture) {
                                dyn.dynamic = true;
                                NSLog(@"texture id %u flagged dynamic: hashing and HD lookup off",
                                      textureUpdate.texture_id);
                            }
                        }
                        if ((dyn.dynamic || cursorTexture) && !hd) {
                            if (shadowTarget) {
                                for (id<MTLTexture> ringTexture : dyn.ring) {
                                    if (ringTexture &&
                                        !(ringTexture.usage & MTLTextureUsageRenderTarget)) {
                                        for (auto &oldTexture : dyn.ring) oldTexture = nil;
                                        break;
                                    }
                                }
                            }
                            id<MTLTexture> current = dyn.ring[dyn.ringIndex];
                            if (current && (current.width != textureUpdate.width ||
                                            current.height != textureUpdate.height)) {
                                for (auto &slot : dyn.ring) slot = nil;
                                current = nil;
                            }
                            dyn.ringIndex = (uint8_t)((dyn.ringIndex + 1) % kFramesInFlight);
                            id<MTLTexture> target = dyn.ring[dyn.ringIndex];
                            if (!target) {
                                MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
                                    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                width:textureUpdate.width
                                                               height:textureUpdate.height
                                                            mipmapped:NO];
                                descriptor.storageMode = MTLStorageModeShared;
                                descriptor.usage = MTLTextureUsageShaderRead |
                                    (shadowTarget ? MTLTextureUsageRenderTarget : 0);
                                target = [_device newTextureWithDescriptor:descriptor];
                                if (target) {
                                    target.label = [NSString
                                            stringWithFormat:@"DK2 dynamic %u/%u",
                                                             textureUpdate.texture_id, dyn.ringIndex];
                                    dyn.ring[dyn.ringIndex] = target;
                                    [_resources addAllocation:target];
                                    residencyChanged = YES;
                                }
                            }
                            if (target) {
                                [target replaceRegion:MTLRegionMake2D(0, 0, textureUpdate.width,
                                                                      textureUpdate.height)
                                          mipmapLevel:0 withBytes:pixels
                                          bytesPerRow:textureUpdate.row_pitch];
                                _textures[key] = target;
                                texdump::dump(pixels, textureUpdate.width, textureUpdate.height,
                                              textureUpdate.row_pitch, textureUpdate.texture_id);
                            }
                        } else if (hd) {
                            installTexture(key, hd);
                        } else {
                        id<MTLTexture> texture = _textures[key];
                        if (!texture || texture.width != textureUpdate.width ||
                            texture.height != textureUpdate.height) {
                            // Static textures carry a full mip chain: the
                            // engine's reduction levels are ignored by the GPU
                            // mesh path, native mip-mapping replaces them.
                            MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
                                texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                            width:textureUpdate.width
                                                           height:textureUpdate.height
                                                        mipmapped:YES];
                            descriptor.storageMode = MTLStorageModeShared;
                            descriptor.usage = MTLTextureUsageShaderRead;
                            texture = [_device newTextureWithDescriptor:descriptor];
                            if (texture) {
                                texture.label = [NSString stringWithFormat:@"DK2 texture %u", textureUpdate.texture_id];
                                installTexture(key, texture);
                            }
                        }
                        if (texture) {
                            [texture replaceRegion:MTLRegionMake2D(0, 0, textureUpdate.width, textureUpdate.height)
                                       mipmapLevel:0 withBytes:pixels bytesPerRow:textureUpdate.row_pitch];
                            texhd::fillMipChain(texture, pixels, textureUpdate.width,
                                         textureUpdate.height, textureUpdate.row_pitch);
                            texdump::dump(pixels, textureUpdate.width, textureUpdate.height,
                                          textureUpdate.row_pitch, textureUpdate.texture_id);
                        }
                        }
                    }
                } else if (view.type == DK2M_COMMAND_TEXTURE_UPDATE_RECT &&
                           view.size >= sizeof(DK2MTextureUpdateRectCommand)) {
                    DK2MTextureUpdateRectCommand textureUpdate;
                    std::memcpy(&textureUpdate, snapshot->bytes.data() + offset,
                                sizeof(textureUpdate));
                    const size_t expected = sizeof(textureUpdate) + textureUpdate.data_size;
                    const auto textureFound = _textures.find(textureUpdate.texture_id);
                    id<MTLTexture> texture = textureFound == _textures.end()
                                                   ? nil : textureFound->second;
                    if (texture && expected <= view.size && textureUpdate.width &&
                        textureUpdate.height && textureUpdate.x <= texture.width &&
                        textureUpdate.y <= texture.height &&
                        textureUpdate.width <= texture.width - textureUpdate.x &&
                        textureUpdate.height <= texture.height - textureUpdate.y &&
                        textureUpdate.row_pitch >= textureUpdate.width * 4 &&
                        textureUpdate.data_size >=
                            static_cast<uint64_t>(textureUpdate.row_pitch) * textureUpdate.height) {
                        ++metrics.textureUpdates;
                        metrics.textureBytes += textureUpdate.data_size;
                        const uint8_t *pixels = snapshot->bytes.data() + offset + sizeof(textureUpdate);
                        if (respack::applyRectToStored(textureUpdate.texture_id,
                                                       textureUpdate.x, textureUpdate.y,
                                                       textureUpdate.width, textureUpdate.height,
                                                       pixels, textureUpdate.row_pitch) &&
                            !shadowTargetIds.contains(textureUpdate.texture_id)) {
                            // The bound texture is a 4x resource-pack page; a
                            // 1x replaceRegion would land at the wrong scale.
                            // Stored pixels were patched; the page rebuilds in
                            // the pass below.
                        } else {
                            [texture replaceRegion:MTLRegionMake2D(
                                                       textureUpdate.x, textureUpdate.y,
                                                       textureUpdate.width, textureUpdate.height)
                                       mipmapLevel:0
                                         withBytes:pixels
                                       bytesPerRow:textureUpdate.row_pitch];
                        }
                    }
                } else if (view.type == DK2M_COMMAND_PAGE_ATLAS_MAP) {
                    // Persistent metadata was consumed by the pre-pass above.
                }
            }
        }
        // Resource-pack pages whose atlas map or content changed this frame
        // (PAGE_ATLAS_MAP after the upload, or a rect update into a mapped
        // page) are rebuilt here from the stored 1x pixels.
        respack::rebuildDirty(_device, &metrics.packHits,
                              &metrics.packPagesComposed, &metrics.packPagesRestored,
                              shadowTargetIds,
                              ^(uint32_t textureId, id<MTLTexture> texture) {
            installTexture(textureId, texture);
        });
        if (residencyChanged) [_resources commit];

        // Rasterize the original game's already-projected silhouettes. Each
        // 32x32 destination pixel corresponds to the same 8x8 subpixel block
        // the CPU path accumulates; the resolve writes back into the normal
        // atlas texture, so DK2's original decal draw remains untouched.
        struct ShadowMaskWork {
            uint32_t textureId;
            id<MTLTexture> target;
            NSUInteger vertexStart;
            NSUInteger vertexCount;
            NSUInteger uniformIndex;
            NSUInteger tile;
        };
        std::vector<ShadowMaskWork> shadowMasks;
        std::vector<uint32_t> shadowTargetOrder;
        NSUInteger shadowVertexBytes = 0;
        const bool shadowResourcesReady = _shadowsAvailable &&
            _shadowScratchTextures[slot] && _shadowMaskArgumentTables[slot] &&
            _shadowResolveArgumentTables[slot];
        metrics.shadowLive = dk2ShadowsEnabled() && shadowResourcesReady;
        if (snapshot) {
            auto *shadowVertices =
                shadowResourcesReady
                    ? static_cast<ShadowMaskVertex *>(_shadowVertexBuffers[slot].contents)
                    : nullptr;
            auto *shadowUniforms =
                shadowResourcesReady
                    ? static_cast<ShadowResolveUniform *>(_shadowResolveBuffers[slot].contents)
                    : nullptr;
            for (const CommandView &view : _commandViews) {
                if (view.type != DK2M_COMMAND_SHADOW_MASK) continue;
                ++metrics.shadowMasksReceived;
                if (view.size < sizeof(DK2MShadowMaskCommand)) {
                    ++metrics.shadowRejectMalformed;
                    continue;
                }
                DK2MShadowMaskCommand shadow{};
                std::memcpy(&shadow, snapshot->bytes.data() + view.offset, sizeof(shadow));
                const uint64_t triangleBytes =
                    static_cast<uint64_t>(shadow.triangle_count) * sizeof(DK2MShadowTriangle);
                if (sizeof(shadow) + triangleBytes > view.size ||
                    shadow.target_width != 32 || shadow.target_height != 32 ||
                    shadow.mode > DK2M_SHADOW_MASK_GRAYSCALE) {
                    ++metrics.shadowRejectMalformed;
                    continue;
                }
                if (!shadowResourcesReady) {
                    ++metrics.shadowRejectUnavailable;
                    continue;
                }
                if (shadowMasks.size() >= kMaxShadowMasksPerFrame) {
                    ++metrics.shadowRejectCapacity;
                    continue;
                }
                const auto found = _textures.find(shadow.texture_id);
                id<MTLTexture> target = found == _textures.end() ? nil : found->second;
                if (!target || target.pixelFormat != MTLPixelFormatBGRA8Unorm ||
                    !(target.usage & MTLTextureUsageRenderTarget) ||
                    shadow.target_x > target.width || shadow.target_y > target.height ||
                    shadow.target_width > target.width - shadow.target_x ||
                    shadow.target_height > target.height - shadow.target_y) {
                    ++metrics.shadowRejectTarget;
                    continue;
                }

                const NSUInteger availableVertices =
                    (kShadowVertexBufferSize - shadowVertexBytes) / sizeof(ShadowMaskVertex);
                const NSUInteger requestedVertices =
                    static_cast<NSUInteger>(shadow.triangle_count) * 3;
                if (requestedVertices > availableVertices) {
                    ++metrics.shadowRejectCapacity;
                    continue;
                }
                const NSUInteger copiedVertices = requestedVertices;
                const auto *triangles = reinterpret_cast<const DK2MShadowTriangle *>(
                    snapshot->bytes.data() + view.offset + sizeof(shadow));
                const NSUInteger vertexStart = shadowVertexBytes / sizeof(ShadowMaskVertex);
                for (NSUInteger triangle = 0; triangle < copiedVertices / 3; ++triangle) {
                    const DK2MShadowTriangle &source = triangles[triangle];
                    shadowVertices[vertexStart + triangle * 3 + 0] =
                        {static_cast<float>(source.x0), static_cast<float>(source.y0)};
                    shadowVertices[vertexStart + triangle * 3 + 1] =
                        {static_cast<float>(source.x1), static_cast<float>(source.y1)};
                    shadowVertices[vertexStart + triangle * 3 + 2] =
                        {static_cast<float>(source.x2), static_cast<float>(source.y2)};
                }
                shadowVertexBytes += copiedVertices * sizeof(ShadowMaskVertex);

                const NSUInteger index = shadowMasks.size();
                const NSUInteger tileX = (index % kShadowTilesPerAxis) * kShadowSubpixelSize;
                const NSUInteger tileY = (index / kShadowTilesPerAxis) * kShadowSubpixelSize;
                shadowUniforms[index] = {
                    static_cast<uint32_t>(tileX), static_cast<uint32_t>(tileY),
                    shadow.target_x, shadow.target_y, shadow.mode, 0, 0, 0};
                shadowMasks.push_back({shadow.texture_id, target, vertexStart,
                                       copiedVertices, index, index});
                if (std::find(shadowTargetOrder.begin(), shadowTargetOrder.end(),
                              shadow.texture_id) == shadowTargetOrder.end()) {
                    shadowTargetOrder.push_back(shadow.texture_id);
                }
            }
        }

        const bool shadowMasksRendered = !shadowMasks.empty();
        if (shadowMasksRendered) {
            MTL4RenderPassDescriptor *maskPass = [[MTL4RenderPassDescriptor alloc] init];
            maskPass.colorAttachments[0].texture = _shadowScratchTextures[slot];
            maskPass.colorAttachments[0].loadAction = MTLLoadActionClear;
            maskPass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
            maskPass.colorAttachments[0].storeAction = MTLStoreActionStore;
            [_shadowMaskArgumentTables[slot]
                setAddress:_shadowVertexBuffers[slot].gpuAddress atIndex:0];
            id<MTL4RenderCommandEncoder> maskEncoder =
                [commandBuffer renderCommandEncoderWithDescriptor:maskPass];
            maskEncoder.label = @"DK2 original shadow masks";
            [maskEncoder setArgumentTable:_shadowMaskArgumentTables[slot]
                                 atStages:MTLRenderStageVertex];
            [maskEncoder setRenderPipelineState:_shadowMaskPipeline];
            [maskEncoder setCullMode:MTLCullModeNone];
            for (const ShadowMaskWork &work : shadowMasks) {
                const NSUInteger tileX =
                    (work.tile % kShadowTilesPerAxis) * kShadowSubpixelSize;
                const NSUInteger tileY =
                    (work.tile / kShadowTilesPerAxis) * kShadowSubpixelSize;
                [maskEncoder setViewport:MTLViewport{
                    static_cast<double>(tileX), static_cast<double>(tileY),
                    static_cast<double>(kShadowSubpixelSize),
                    static_cast<double>(kShadowSubpixelSize), 0.0, 1.0}];
                [maskEncoder setScissorRect:MTLScissorRect{
                    tileX, tileY, kShadowSubpixelSize, kShadowSubpixelSize}];
                if (work.vertexCount) {
                    [maskEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                                    vertexStart:work.vertexStart
                                    vertexCount:work.vertexCount];
                }
            }
            [maskEncoder endEncoding];

            [_shadowResolveArgumentTables[slot]
                setAddress:_shadowResolveBuffers[slot].gpuAddress atIndex:0];
            [_shadowResolveArgumentTables[slot]
                setTexture:_shadowScratchTextures[slot].gpuResourceID atIndex:0];
            bool firstResolve = true;
            for (uint32_t targetId : shadowTargetOrder) {
                const auto targetWork = std::find_if(
                    shadowMasks.begin(), shadowMasks.end(),
                    [targetId](const ShadowMaskWork &work) {
                        return work.textureId == targetId;
                    });
                if (targetWork == shadowMasks.end()) continue;
                MTL4RenderPassDescriptor *resolvePass = [[MTL4RenderPassDescriptor alloc] init];
                resolvePass.colorAttachments[0].texture = targetWork->target;
                resolvePass.colorAttachments[0].loadAction = MTLLoadActionLoad;
                resolvePass.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTL4RenderCommandEncoder> resolveEncoder =
                    [commandBuffer renderCommandEncoderWithDescriptor:resolvePass];
                resolveEncoder.label = @"DK2 shadow atlas resolve";
                if (firstResolve) {
                    [resolveEncoder barrierAfterQueueStages:MTLStageFragment
                                               beforeStages:MTLStageFragment
                                          visibilityOptions:MTL4VisibilityOptionDevice];
                    firstResolve = false;
                }
                [resolveEncoder setArgumentTable:_shadowResolveArgumentTables[slot]
                                         atStages:MTLRenderStageFragment];
                [resolveEncoder setRenderPipelineState:_shadowResolvePipeline];
                [resolveEncoder setCullMode:MTLCullModeNone];
                for (const ShadowMaskWork &work : shadowMasks) {
                    if (work.textureId != targetId) continue;
                    const ShadowResolveUniform &uniform =
                        static_cast<const ShadowResolveUniform *>(
                            _shadowResolveBuffers[slot].contents)[work.uniformIndex];
                    [resolveEncoder setViewport:MTLViewport{
                        static_cast<double>(uniform.targetX),
                        static_cast<double>(uniform.targetY), 32.0, 32.0, 0.0, 1.0}];
                    [resolveEncoder setScissorRect:MTLScissorRect{
                        uniform.targetX, uniform.targetY, 32, 32}];
                    [resolveEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                                       vertexStart:0 vertexCount:3
                                     instanceCount:1 baseInstance:work.uniformIndex];
                    ++metrics.shadowMasksRendered;
                    metrics.shadowTriangles += static_cast<uint32_t>(work.vertexCount / 3);
                }
                [resolveEncoder endEncoding];
            }
        }

        // Immediate proof for live A/B: settings logs show intent, this log
        // shows that the host actually encoded the GPU mask and atlas passes.
        static bool previousShadowLive = false;
        static bool shadowLiveConfirmed = false;
        static bool shadowRejectReported = false;
        const bool shadowLive = metrics.shadowLive != 0;
        if (shadowLive != previousShadowLive) {
            NSLog(@"DK2 shadows GPU path -> %s", shadowLive ? "on; awaiting masks" : "off; CPU fallback");
            previousShadowLive = shadowLive;
            shadowLiveConfirmed = false;
            shadowRejectReported = false;
        }
        if (shadowLive && metrics.shadowMasksRendered && !shadowLiveConfirmed) {
            NSLog(@"DK2 shadows GPU activity confirmed: received=%u rendered=%u triangles=%u",
                  metrics.shadowMasksReceived, metrics.shadowMasksRendered,
                  metrics.shadowTriangles);
            shadowLiveConfirmed = true;
        } else if (shadowLive && metrics.shadowMasksReceived &&
                   !metrics.shadowMasksRendered && !shadowRejectReported) {
            NSLog(@"DK2 shadows GPU commands rejected: unavailable=%u malformed=%u target=%u capacity=%u",
                  metrics.shadowRejectUnavailable, metrics.shadowRejectMalformed,
                  metrics.shadowRejectTarget, metrics.shadowRejectCapacity);
            shadowRejectReported = true;
        }

        MTL4RenderPassDescriptor *pass = [[MTL4RenderPassDescriptor alloc] init];
        MTLRenderPassColorAttachmentDescriptor *color = pass.colorAttachments[0];
        const BOOL bloomActive = dk2BloomEnabled() && _bloomAvailable &&
            _sceneColorTexture != nil && _bloomTextureA != nil && _bloomTextureB != nil &&
            _bloomArgumentTables[slot] != nil;
        color.texture = _multisampleColorTexture;
        color.resolveTexture = bloomActive ? _sceneColorTexture : update.drawable.texture;
        color.loadAction = MTLLoadActionClear;
        color.storeAction = MTLStoreActionMultisampleResolve;
        color.clearColor = clearColor;
        MTLRenderPassDepthAttachmentDescriptor *depth = pass.depthAttachment;
        depth.texture = _depthTexture;
        depth.loadAction = MTLLoadActionClear;
        depth.storeAction = MTLStoreActionDontCare;
        depth.clearDepth = 1.0;

        id<MTL4RenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        encoder.label = @"DK2 native Metal frame";
        if (shadowMasksRendered) {
            // The resolve pass writes the same atlas textures sampled below.
            // Metal 4 resources are untracked, so make those writes visible.
            [encoder barrierAfterQueueStages:MTLStageFragment
                                 beforeStages:MTLStageFragment
                            visibilityOptions:MTL4VisibilityOptionDevice];
        }
        [encoder setDepthStencilState:_depthStates[4][1]];
        if (snapshot) {
            NSUInteger vertexOffset = 0;
            NSUInteger indexOffset = 0;
            NSUInteger drawUniformCount = 0;
            auto *drawUniforms = static_cast<DrawUniform *>(_drawBuffers[slot].contents);
            TextureBinding currentTextureBinding = {0, 0};
            TextureBinding currentTextureBinding1 = {0, 0};
            TextureBinding currentTextureBinding2 = {0, 0};
            NSUInteger nextSlot[kTextureArgumentTablesPerFrame];
            for (NSUInteger bank = 0; bank < kTextureArgumentTablesPerFrame; ++bank) nextSlot[bank] = 1;
            uint32_t boundArgumentTableBank = 0;
            uint32_t zFunction = 4;
            BOOL zEnabled = YES;
            BOOL zWriteEnabled = YES;
            BOOL alphaBlendEnabled = NO;
            uint32_t sourceBlend = 2;
            uint32_t destinationBlend = 1;
            uint32_t cullMode = 1;
            uint32_t textureFactor = 0xFFFFFFFFu;
            uint32_t textureStage0[7] = {0, 4, 2, 0, 4, 2, 0};
            // colorOp/alphaOp default to D3DTOP_DISABLE (1): a draw that never
            // sets stage-1 state renders exactly as before (single texture).
            uint32_t textureStage1[7] = {0, 1, 2, 1, 1, 2, 1};
            uint32_t textureStage2[7] = {0, 1, 2, 1, 1, 2, 1};
            // D3DTSS_BUMPENVMAT00/01/10/11 = states 7/8/9/10, BUMPENVLSCALE/
            // LOFFSET = states 22/23. Indexed [stage][0..3] = matrix,
            // [stage][4] = LScale, [stage][5] = LOffset. Identity/neutral
            // defaults so an unused bump stage is inert if ever read.
            float bumpEnv[2][6] = {{1, 0, 0, 1, 1, 0}, {1, 0, 0, 1, 1, 0}};
            _frameTextureBindings.clear();
            // Binds `textureId` into `bank` specifically and records it, or
            // returns nullopt if that one bank has no free slot left (the
            // caller decides the fallback - never silently picks another
            // bank, which is exactly what used to make stage 1's texture
            // land in a different bank than stage 0's for the same draw).
            auto allocateInBank = [&](uint32_t textureId, uint16_t bank) -> std::optional<TextureBinding> {
                if (nextSlot[bank] >= kTextureBindingsPerArgumentTable) return std::nullopt;
                const auto textureFound = _textures.find(textureId);
                id<MTLTexture> texture = textureFound == _textures.end() ? nil : textureFound->second;
                if (!texture) {
                    ++metrics.missingTextures;
                    return std::nullopt;
                }
                const TextureBinding binding = {bank, static_cast<uint16_t>(nextSlot[bank])};
                _frameTextureBindings.push_back({textureId, binding});
                [_argumentTables[slot][bank] setTexture:texture.gpuResourceID atIndex:binding.slot];
                ++nextSlot[bank];
                return binding;
            };
            // Stage 0 (unconstrained): reuse an existing binding in any bank,
            // else allocate in the first bank with room. A texture that does
            // not fit anywhere falls back to the shared white slot {0, 0}.
            //
            // Tried "least-full bank" here once, reasoning it would leave more
            // headroom for stage 1/2 to land alongside stage 0 in the same
            // bank. It measurably made things worse: stage 0/1/2 textures for
            // one draw are bound via separate SET_TEXTURE commands at
            // different times, and first-fit's tendency to keep filling
            // "the current bank" until it's full is what accidentally kept
            // temporally-close textures (i.e. textures actually used together
            // by the same draws) clustered in the same bank. Spreading new
            // textures evenly instead scattered them, so stage 1/2 missed
            // stage 0's bank far more often - binding-overflow went from
            // ~25k-90k to 30k-220k per window. Reverted.
            auto resolveTextureBinding = [&](uint32_t textureId) -> TextureBinding {
                const auto found = std::find_if(
                    _frameTextureBindings.begin(), _frameTextureBindings.end(),
                    [&](const TextureBindingEntry &entry) { return entry.textureId == textureId; });
                if (found != _frameTextureBindings.end()) return found->binding;
                for (uint16_t bank = 0; bank < kTextureArgumentTablesPerFrame; ++bank) {
                    if (const auto binding = allocateInBank(textureId, bank)) return *binding;
                }
                ++metrics.bindingOverflows;
                return {0, 0};
            };
            // Stage 1 (bank-constrained to `bank`, which stage 0 already
            // committed to for this draw): reuse an existing binding in THAT
            // bank, else allocate a fresh slot in it. If the bank is full,
            // fall back to white IN THAT SAME BANK ({0, bank} still samples
            // the shared white texture, never a cross-bank slot) instead of
            // spilling into a different bank the encoder isn't looking at.
            auto resolveTextureBindingInBank = [&](uint32_t textureId, uint16_t bank) -> TextureBinding {
                const auto found = std::find_if(
                    _frameTextureBindings.begin(), _frameTextureBindings.end(),
                    [&](const TextureBindingEntry &entry) {
                        return entry.textureId == textureId && entry.binding.bank == bank;
                    });
                if (found != _frameTextureBindings.end()) return found->binding;
                if (const auto binding = allocateInBank(textureId, bank)) return *binding;
                ++metrics.bindingOverflows;
                return {0, bank};
            };
            for (NSUInteger bank = 0; bank < kTextureArgumentTablesPerFrame; ++bank) {
                [_argumentTables[slot][bank]
                    setAddress:_vertexBuffers[slot].gpuAddress atIndex:0];
                [_argumentTables[slot][bank]
                    setAddress:_drawBuffers[slot].gpuAddress atIndex:1];
                [_argumentTables[slot][bank]
                    setAddress:_meshVertexBuffers[slot].gpuAddress atIndex:2];
                [_argumentTables[slot][bank]
                    setAddress:_cameraBuffers[slot].gpuAddress atIndex:3];
                [_argumentTables[slot][bank]
                    setAddress:_lightsBuffers[slot].gpuAddress atIndex:4];
                [_argumentTables[slot][bank]
                    setAddress:_lightsBuffers[slot].gpuAddress + kLightsHeaderBytes atIndex:5];
                [_argumentTables[slot][bank]
                    setAddress:_meshDrawBuffers[slot].gpuAddress atIndex:6];
                [_argumentTables[slot][bank]
                    setTexture:_whiteTexture.gpuResourceID atIndex:0];
                [_argumentTables[slot][bank]
                    setSamplerState:_sampler.gpuResourceID atIndex:0];
            }
            // Per-frame mesh state: identity camera and zero lights until the
            // producer supplies them, plus per-frame placement of referenced
            // meshes in the mesh vertex/index streams.
            {
                float *cam = static_cast<float *>(_cameraBuffers[slot].contents);
                std::memset(cam, 0, kCameraBufferSize);
                cam[0] = cam[5] = cam[10] = cam[15] = 1.0f;
                std::memset(_lightsBuffers[slot].contents, 0, 16);
            }
            NSUInteger meshVertexOffset = 0;
            NSUInteger meshDrawCount = 0;
            struct MeshPlacement { NSUInteger baseVertex; NSUInteger indexByteOffset; uint32_t indexCount; };
            std::unordered_map<uint32_t, MeshPlacement> meshPlacements;
            [encoder setArgumentTable:_argumentTables[slot][boundArgumentTableBank]
                             atStages:MTLRenderStageVertex | MTLRenderStageFragment];
            // Two passes over the stream: mesh-pipeline commands encode FIRST
            // (opaque, z-tested - hoisting them is order-safe versus other 3D
            // draws), so the legacy stream - including the game's HUD quads
            // drawn late with depth off - always lands on top. Without this,
            // scene-phase mesh buckets flushed at frame finish painted the
            // terrain over the UI.
            for (int meshPass = 1; meshPass >= 0; --meshPass) {
            for (const CommandView &view : _commandViews) {
                const size_t commandOffset = view.offset;
                const bool isMeshCommand =
                    view.type == DK2M_COMMAND_MESH_REGISTER ||
                    view.type == DK2M_COMMAND_CAMERA_SET ||
                    view.type == DK2M_COMMAND_LIGHTS_SET ||
                    view.type == DK2M_COMMAND_DRAW_MESH ||
                    view.type == DK2M_COMMAND_DRAW_MESH_INLINE;
                if (isMeshCommand != (meshPass == 1)) continue;
                if (view.type == DK2M_COMMAND_SET_TEXTURE &&
                    view.size == sizeof(DK2MSetTextureCommand)) {
                    DK2MSetTextureCommand binding;
                    std::memcpy(&binding, snapshot->bytes.data() + commandOffset, sizeof(binding));
                    if (binding.stage == 0) {
                        currentTextureBinding = binding.texture_id
                            ? resolveTextureBinding(binding.texture_id) : TextureBinding{0, 0};
                    } else if (binding.stage == 1) {
                        currentTextureBinding1 = binding.texture_id
                            ? resolveTextureBindingInBank(binding.texture_id, currentTextureBinding.bank)
                            : TextureBinding{currentTextureBinding.bank, 0};
                    } else if (binding.stage == 2) {
                        currentTextureBinding2 = binding.texture_id
                            ? resolveTextureBindingInBank(binding.texture_id, currentTextureBinding.bank)
                            : TextureBinding{currentTextureBinding.bank, 0};
                    }
                } else if (view.type == DK2M_COMMAND_RENDER_STATE &&
                           view.size == sizeof(DK2MRenderStateCommand)) {
                    DK2MRenderStateCommand state;
                    std::memcpy(&state, snapshot->bytes.data() + commandOffset, sizeof(state));
                    switch (state.state) {
                        case kD3DRenderStateZEnable: zEnabled = state.value != 0; break;
                        case kD3DRenderStateZWriteEnable: zWriteEnabled = state.value != 0; break;
                        case kD3DRenderStateCullMode: cullMode = state.value; break;
                        case kD3DRenderStateZFunc:
                            if (state.value >= 1 && state.value <= 8) zFunction = state.value;
                            break;
                        case kD3DRenderStateSourceBlend: sourceBlend = state.value; break;
                        case kD3DRenderStateDestinationBlend: destinationBlend = state.value; break;
                        case kD3DRenderStateAlphaBlendEnable: alphaBlendEnabled = state.value != 0; break;
                        case kD3DRenderStateTextureFactor: textureFactor = state.value; break;
                        default: break;
                    }
                } else if (view.type == DK2M_COMMAND_TEXTURE_STAGE_STATE &&
                           view.size == sizeof(DK2MTextureStageStateCommand)) {
                    DK2MTextureStageStateCommand state;
                    std::memcpy(&state, snapshot->bytes.data() + commandOffset, sizeof(state));
                    if (state.state >= 1 && state.state <= 6) {
                        if (state.stage == 0) textureStage0[state.state] = state.value;
                        else if (state.stage == 1) textureStage1[state.state] = state.value;
                        else if (state.stage == 2) textureStage2[state.state] = state.value;
                    } else if (state.state >= 7 && state.state <= 10 && state.stage <= 1) {
                        // D3DTSS_BUMPENVMAT00/01/10/11: only stage 0/1 can carry a
                        // bump op (stage 2 is always terminal).
                        float value;
                        std::memcpy(&value, &state.value, sizeof(value));
                        bumpEnv[state.stage][state.state - 7] = value;
                    } else if ((state.state == 22 || state.state == 23) && state.stage <= 1) {
                        // D3DTSS_BUMPENVLSCALE=22, D3DTSS_BUMPENVLOFFSET=23.
                        float value;
                        std::memcpy(&value, &state.value, sizeof(value));
                        bumpEnv[state.stage][state.state == 22 ? 4 : 5] = value;
                    } else {
                        // ponytail: one-shot NSLog per distinct (stage, state) the
                        // combiner still doesn't model (e.g. D3DTSS_TEXCOORDINDEX=11,
                        // MAGFILTER/MINFILTER=16/17, TEXTURETRANSFORMFLAGS=24).
                        static std::unordered_set<uint32_t> seenStageStates;
                        const uint32_t key = (state.stage << 16) | state.state;
                        if (seenStageStates.insert(key).second) {
                            NSLog(@"DIAG unhandled texture-stage-state: stage=%u state=%u value=%u",
                                  state.stage, state.state, state.value);
                        }
                    }
                } else if (view.type == DK2M_COMMAND_MESH_REGISTER &&
                           view.size >= sizeof(DK2MMeshRegisterCommand)) {
                    DK2MMeshRegisterCommand reg;
                    std::memcpy(&reg, snapshot->bytes.data() + commandOffset, sizeof(reg));
                    const size_t vertexBytes = static_cast<size_t>(reg.vertex_count) * kMeshVertexStride;
                    const size_t indexBytes = (static_cast<size_t>(reg.index_count) * 2 + 3) & ~static_cast<size_t>(3);
                    if (reg.mesh_id && reg.vertex_count && reg.index_count &&
                        sizeof(reg) + vertexBytes + indexBytes <= view.size) {
                        MeshBlob &blob = _meshes[reg.mesh_id];
                        if (blob.vertices.empty()) {
                            const uint8_t *payload = snapshot->bytes.data() + commandOffset + sizeof(reg);
                            blob.vertices.assign(payload, payload + vertexBytes);
                            blob.indices.assign(payload + vertexBytes,
                                                payload + vertexBytes + static_cast<size_t>(reg.index_count) * 2);
                            blob.vertexCount = reg.vertex_count;
                            blob.indexCount = reg.index_count;
                        }
                    }
                } else if (view.type == DK2M_COMMAND_CAMERA_SET &&
                           view.size == sizeof(DK2MCameraSetCommand)) {
                    DK2MCameraSetCommand camera;
                    std::memcpy(&camera, snapshot->bytes.data() + commandOffset, sizeof(camera));
                    std::memcpy(_cameraBuffers[slot].contents, camera.view_proj,
                                24 * sizeof(float));
                } else if (view.type == DK2M_COMMAND_LIGHTS_SET &&
                           view.size >= sizeof(DK2MLightsSetCommand)) {
                    DK2MLightsSetCommand lightsCommand;
                    std::memcpy(&lightsCommand, snapshot->bytes.data() + commandOffset,
                                sizeof(lightsCommand));
                    const size_t lutBytes = 256 * sizeof(float);
                    const size_t lightBytes = static_cast<size_t>(lightsCommand.light_count) * 48;
                    if (sizeof(lightsCommand) + lutBytes + lightBytes <= view.size) {
                        const uint32_t count = std::min<uint32_t>(
                            lightsCommand.light_count, static_cast<uint32_t>(kMaxLightsPerFrame));
                        auto *dst = static_cast<uint8_t *>(_lightsBuffers[slot].contents);
                        struct { uint32_t count; float r, g, b; } lightsHeader = {
                            count, lightsCommand.ambient_r, lightsCommand.ambient_g,
                            lightsCommand.ambient_b};
                        std::memcpy(dst, &lightsHeader, sizeof(lightsHeader));
                        static NSTimeInterval lastLightsLog = 0;
                        const NSTimeInterval nowLights = CACurrentMediaTime();
                        if (nowLights - lastLightsLog > 3.0) {
                            lastLightsLog = nowLights;
                            NSLog(@"DIAG lights received: count=%u ambient=(%.3f %.3f %.3f)",
                                  count, lightsCommand.ambient_r, lightsCommand.ambient_g,
                                  lightsCommand.ambient_b);
                        }
                        const uint8_t *payload =
                            snapshot->bytes.data() + commandOffset + sizeof(lightsCommand);
                        std::memcpy(dst + 16, payload, lutBytes);
                        std::memcpy(dst + kLightsHeaderBytes, payload + lutBytes,
                                    static_cast<size_t>(count) * 48);
                    }
                } else if (view.type == DK2M_COMMAND_DRAW_MESH &&
                           view.size == sizeof(DK2MDrawMeshCommand)) {
                    DK2MDrawMeshCommand meshDraw;
                    std::memcpy(&meshDraw, snapshot->bytes.data() + commandOffset, sizeof(meshDraw));
                    const auto meshFound = _meshes.find(meshDraw.mesh_id);
                    if (meshFound == _meshes.end() || meshFound->second.vertices.empty()) {
                        ++metrics.invalidDraws;
                    } else {
                        MeshBlob &blob = meshFound->second;
                        auto placed = meshPlacements.find(meshDraw.mesh_id);
                        if (placed == meshPlacements.end()) {
                            const NSUInteger vertexBytes = blob.vertices.size();
                            const NSUInteger indexBytes = blob.indices.size();
                            const NSUInteger alignedIndexOffset = (indexOffset + 3u) & ~static_cast<NSUInteger>(3u);
                            if (meshVertexOffset + vertexBytes <= kMeshVertexBufferSize &&
                                alignedIndexOffset + indexBytes <= kIndexBufferSize) {
                                std::memcpy(static_cast<uint8_t *>(_meshVertexBuffers[slot].contents) +
                                                meshVertexOffset,
                                            blob.vertices.data(), vertexBytes);
                                std::memcpy(static_cast<uint8_t *>(_indexBuffers[slot].contents) +
                                                alignedIndexOffset,
                                            blob.indices.data(), indexBytes);
                                const MeshPlacement placement = {
                                    meshVertexOffset / kMeshVertexStride, alignedIndexOffset,
                                    blob.indexCount};
                                indexOffset = alignedIndexOffset + ((indexBytes + 3u) & ~static_cast<NSUInteger>(3u));
                                meshVertexOffset += vertexBytes;
                                placed = meshPlacements.emplace(meshDraw.mesh_id, placement).first;
                            }
                        }
                        if (placed == meshPlacements.end() || meshDrawCount >= kMaxMeshDrawsPerFrame ||
                            !snapshot->width || !snapshot->height) {
                            ++metrics.invalidDraws;
                        } else {
                            const TextureBinding binding = meshDraw.texture_id
                                ? resolveTextureBinding(meshDraw.texture_id)
                                : TextureBinding{static_cast<uint16_t>(boundArgumentTableBank), 0};
                            auto *uniforms =
                                static_cast<MeshDrawUniform *>(_meshDrawBuffers[slot].contents);
                            MeshDrawUniform &uniform = uniforms[meshDrawCount];
                            std::memcpy(uniform.world0, meshDraw.world + 0, 16);
                            std::memcpy(uniform.world1, meshDraw.world + 4, 16);
                            std::memcpy(uniform.world2, meshDraw.world + 8, 16);
                            uniform.ambient[0] = meshDraw.ambient_r;
                            uniform.ambient[1] = meshDraw.ambient_g;
                            uniform.ambient[2] = meshDraw.ambient_b;
                            uniform.ambient[3] = 0.0f;
                            uniform.textureIndex = binding.slot;
                            uniform.tint = meshDraw.tint;
                            uniform.flags = meshDraw.flags;
                            uniform.pad = 0;
                            id<MTLRenderPipelineState> pipeline = _meshOpaquePipeline;
                            if (alphaBlendEnabled || (meshDraw.flags & 2u)) {
                                pipeline = sourceBlend == 2 && destinationBlend == 2
                                               ? _meshAdditivePipeline : _meshAlphaPipeline;
                            }
                            [encoder setRenderPipelineState:pipeline];
                            const uint32_t effectiveZFunction = zEnabled ? zFunction : 8;
                            const uint32_t effectiveZWrite = zEnabled && zWriteEnabled ? 1 : 0;
                            [encoder setDepthStencilState:_depthStates[effectiveZFunction][effectiveZWrite]];
                            [encoder setCullMode:cullMode == 1 ? MTLCullModeNone : MTLCullModeBack];
                            [encoder setFrontFacingWinding:cullMode == 3 ? MTLWindingClockwise
                                                                        : MTLWindingCounterClockwise];
                            if (boundArgumentTableBank != binding.bank) {
                                boundArgumentTableBank = binding.bank;
                                [encoder setArgumentTable:_argumentTables[slot][boundArgumentTableBank]
                                                 atStages:MTLRenderStageVertex | MTLRenderStageFragment];
                            }
                            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                                indexCount:placed->second.indexCount
                                                 indexType:MTLIndexTypeUInt16
                                               indexBuffer:_indexBuffers[slot].gpuAddress +
                                                           placed->second.indexByteOffset
                                         indexBufferLength:placed->second.indexCount * 2
                                             instanceCount:1
                                                baseVertex:placed->second.baseVertex
                                              baseInstance:meshDrawCount];
                            ++meshDrawCount;
                            ++metrics.drawCalls;
                            metrics.vertices += blob.vertexCount;
                            metrics.indices += blob.indexCount;
                        }
                    }
                } else if (view.type == DK2M_COMMAND_DRAW_MESH_INLINE &&
                           view.size >= sizeof(DK2MDrawMeshInlineCommand)) {
                    DK2MDrawMeshInlineCommand inlineDraw;
                    std::memcpy(&inlineDraw, snapshot->bytes.data() + commandOffset,
                                sizeof(inlineDraw));
                    const size_t vertexBytes =
                        static_cast<size_t>(inlineDraw.vertex_count) * kMeshVertexStride;
                    const size_t indexBytes =
                        (static_cast<size_t>(inlineDraw.index_count) * 2 + 3) & ~static_cast<size_t>(3);
                    const NSUInteger alignedIndexOffset = (indexOffset + 3u) & ~static_cast<NSUInteger>(3u);
                    if (!inlineDraw.vertex_count || !inlineDraw.index_count ||
                        sizeof(inlineDraw) + vertexBytes + indexBytes > view.size ||
                        meshVertexOffset + vertexBytes > kMeshVertexBufferSize ||
                        alignedIndexOffset + indexBytes > kIndexBufferSize ||
                        meshDrawCount >= kMaxMeshDrawsPerFrame ||
                        !snapshot->width || !snapshot->height) {
                        ++metrics.invalidDraws;
                    } else {
                        const uint8_t *payload =
                            snapshot->bytes.data() + commandOffset + sizeof(inlineDraw);
                        std::memcpy(static_cast<uint8_t *>(_meshVertexBuffers[slot].contents) +
                                        meshVertexOffset,
                                    payload, vertexBytes);
                        std::memcpy(static_cast<uint8_t *>(_indexBuffers[slot].contents) +
                                        alignedIndexOffset,
                                    payload + vertexBytes, indexBytes);
                        const NSUInteger baseVertex = meshVertexOffset / kMeshVertexStride;
                        meshVertexOffset += vertexBytes;
                        indexOffset = alignedIndexOffset + indexBytes;
                        static NSTimeInterval lastInlineLog = 0;
                        const NSTimeInterval nowInline = CACurrentMediaTime();
                        if (nowInline - lastInlineLog > 3.0) {
                            lastInlineLog = nowInline;
                            float v0[9];
                            std::memcpy(v0, payload, sizeof(v0));
                            uint32_t c0;
                            std::memcpy(&c0, payload + 32, 4);
                            NSLog(@"DIAG inline draw received: verts=%u idx=%u flags=%u tex=%u "
                                  "tint=%08X amb=(%.3f %.3f %.3f) v0.pos=(%.2f %.2f %.2f) v0.col=%08X",
                                  inlineDraw.vertex_count, inlineDraw.index_count,
                                  inlineDraw.flags, inlineDraw.texture_id, inlineDraw.tint,
                                  inlineDraw.ambient_r, inlineDraw.ambient_g, inlineDraw.ambient_b,
                                  v0[0], v0[1], v0[2], c0);
                        }
                        static const bool meshDebug = getenv("DK2_MESH_DEBUG") != nullptr;
                        static const bool meshNoTexture = meshDebug ||
                            getenv("DK2_MESH_NO_TEXTURE") != nullptr;
                        const TextureBinding binding = (!meshNoTexture && inlineDraw.texture_id)
                            ? resolveTextureBinding(inlineDraw.texture_id)
                            : TextureBinding{static_cast<uint16_t>(boundArgumentTableBank), 0};
                        auto *uniforms =
                            static_cast<MeshDrawUniform *>(_meshDrawBuffers[slot].contents);
                        MeshDrawUniform &uniform = uniforms[meshDrawCount];
                        static const float kIdentityRows[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
                        std::memcpy(uniform.world0, kIdentityRows + 0, 16);
                        std::memcpy(uniform.world1, kIdentityRows + 4, 16);
                        std::memcpy(uniform.world2, kIdentityRows + 8, 16);
                        uniform.ambient[0] = inlineDraw.ambient_r;
                        uniform.ambient[1] = inlineDraw.ambient_g;
                        uniform.ambient[2] = inlineDraw.ambient_b;
                        uniform.ambient[3] = 0.0f;
                        uniform.textureIndex = binding.slot;
                        uniform.tint = meshDebug ? 0xFFFFFFFFu : inlineDraw.tint;
                        uniform.flags = inlineDraw.flags;
                        uniform.pad = 0;
                        // pipeline strictly from the draw's own flags: mesh
                        // commands sit at the frame head and must not inherit
                        // whatever blend state the previous frame replay left on
                        id<MTLRenderPipelineState> pipeline = _meshOpaquePipeline;
                        if (!meshDebug) {
                            if (inlineDraw.flags & 4u) pipeline = _meshAdditivePipeline;
                            else if (inlineDraw.flags & 2u) pipeline = _meshAlphaPipeline;
                            // flag 8 (alpha test) stays on the opaque pipeline:
                            // the shader discards sub-reference texels
                        }
                        [encoder setRenderPipelineState:pipeline];
                        const uint32_t effectiveZFunction = meshDebug ? 8 : (zEnabled ? zFunction : 8);
                        const uint32_t effectiveZWrite = meshDebug ? 0 : (zEnabled && zWriteEnabled ? 1 : 0);
                        [encoder setDepthStencilState:_depthStates[effectiveZFunction][effectiveZWrite]];
                        [encoder setCullMode:meshDebug ? MTLCullModeNone
                                                       : (cullMode == 1 ? MTLCullModeNone : MTLCullModeBack)];
                        [encoder setFrontFacingWinding:cullMode == 3 ? MTLWindingClockwise
                                                                    : MTLWindingCounterClockwise];
                        if (boundArgumentTableBank != binding.bank) {
                            boundArgumentTableBank = binding.bank;
                            [encoder setArgumentTable:_argumentTables[slot][boundArgumentTableBank]
                                             atStages:MTLRenderStageVertex | MTLRenderStageFragment];
                        }
                        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                            indexCount:inlineDraw.index_count
                                             indexType:MTLIndexTypeUInt16
                                           indexBuffer:_indexBuffers[slot].gpuAddress +
                                                       alignedIndexOffset
                                     indexBufferLength:inlineDraw.index_count * 2
                                         instanceCount:1
                                            baseVertex:baseVertex
                                          baseInstance:meshDrawCount];
                        ++meshDrawCount;
                        ++metrics.drawCalls;
                        metrics.vertices += inlineDraw.vertex_count;
                        metrics.indices += inlineDraw.index_count;
                    }
                } else if (view.type == DK2M_COMMAND_DRAW_INDEXED &&
                           view.size >= sizeof(DK2MDrawIndexedCommand)) {
                    DK2MDrawIndexedCommand draw;
                    std::memcpy(&draw, snapshot->bytes.data() + commandOffset, sizeof(draw));
                    const size_t bridgeVertexSize = draw.fvf == DK2M_FVF_VERTEX1C
                                                        ? sizeof(DK2MVertex1C)
                                                        : draw.fvf == DK2M_FVF_VERTEX2C
                                                              ? sizeof(DK2MVertex2C) : 0;
                    const size_t vertexBytes = static_cast<size_t>(draw.vertex_count) * bridgeVertexSize;
                    const size_t indexBytes = static_cast<size_t>(draw.index_count) * sizeof(uint16_t);
                    const size_t expected = sizeof(draw) + vertexBytes + indexBytes;
                    ++metrics.drawCalls;
                    metrics.vertices += draw.vertex_count;
                    metrics.indices += draw.index_count;
                    if (draw.fvf == DK2M_FVF_VERTEX1C) ++metrics.fvf1Draws;
                    else if (draw.fvf == DK2M_FVF_VERTEX2C) ++metrics.fvf2Draws;
                    const NSUInteger alignedVertexOffset = bridgeVertexSize
                        ? (vertexOffset + bridgeVertexSize - 1) / bridgeVertexSize * bridgeVertexSize
                        : vertexOffset;
                    indexOffset = (indexOffset + 3u) & ~3u;
                    if (bridgeVertexSize && expected <= view.size &&
                        alignedVertexOffset + vertexBytes <= kVertexBufferSize &&
                        indexOffset + indexBytes <= kIndexBufferSize &&
                        drawUniformCount < kMaxDrawsPerFrame &&
                        snapshot->width && snapshot->height) {
                        const uint8_t *rawVertices = snapshot->bytes.data() + commandOffset + sizeof(draw);
                        const uint8_t *rawIndices = rawVertices + vertexBytes;
                        std::memcpy(static_cast<uint8_t *>(_vertexBuffers[slot].contents) +
                                        alignedVertexOffset,
                                    rawVertices, vertexBytes);
                        std::memcpy(static_cast<uint8_t *>(_indexBuffers[slot].contents) + indexOffset,
                                    rawIndices, indexBytes);
                        DrawUniform &uniform = drawUniforms[drawUniformCount];
                        uniform.screenWidth = static_cast<float>(snapshot->width);
                        uniform.screenHeight = static_cast<float>(snapshot->height);
                        uniform.worldGeometry = 0;
                        uniform.textureIndex = currentTextureBinding.slot;
                        uniform.colorOp = textureStage0[1];
                        uniform.colorArg1 = textureStage0[2];
                        uniform.colorArg2 = textureStage0[3];
                        uniform.alphaOp = textureStage0[4];
                        uniform.alphaArg1 = textureStage0[5];
                        uniform.alphaArg2 = textureStage0[6];
                        uniform.textureFactor = textureFactor;
                        // Stage 1 shares argument-table banks with stage 0, but
                        // only one bank is bound to the encoder per draw. If
                        // this draw's two textures landed in different banks
                        // (possible once a busy frame has filled bank 0 with
                        // 127+ distinct textures), disable stage 1 for just
                        // this draw rather than sample the wrong bank's slot -
                        // identical to today's single-texture look, never worse.
                        if (currentTextureBinding1.bank == currentTextureBinding.bank) {
                            uniform.textureIndex1 = currentTextureBinding1.slot;
                            uniform.colorOp1 = textureStage1[1];
                            uniform.colorArg1_1 = textureStage1[2];
                            uniform.colorArg2_1 = textureStage1[3];
                            uniform.alphaOp1 = textureStage1[4];
                            uniform.alphaArg1_1 = textureStage1[5];
                            uniform.alphaArg2_1 = textureStage1[6];
                        } else {
                            uniform.textureIndex1 = 0;
                            uniform.colorOp1 = 1;    // D3DTOP_DISABLE
                            uniform.colorArg1_1 = 2;
                            uniform.colorArg2_1 = 1;
                            uniform.alphaOp1 = 1;    // D3DTOP_DISABLE
                            uniform.alphaArg1_1 = 2;
                            uniform.alphaArg2_1 = 1;
                            ++metrics.bindingOverflows;
                        }
                        // Stage 2: same bank-match rule as stage 1.
                        if (currentTextureBinding2.bank == currentTextureBinding.bank) {
                            uniform.textureIndex2 = currentTextureBinding2.slot;
                            uniform.colorOp2 = textureStage2[1];
                            uniform.colorArg1_2 = textureStage2[2];
                            uniform.colorArg2_2 = textureStage2[3];
                            uniform.alphaOp2 = textureStage2[4];
                            uniform.alphaArg1_2 = textureStage2[5];
                            uniform.alphaArg2_2 = textureStage2[6];
                        } else {
                            uniform.textureIndex2 = 0;
                            uniform.colorOp2 = 1;    // D3DTOP_DISABLE
                            uniform.colorArg1_2 = 2;
                            uniform.colorArg2_2 = 1;
                            uniform.alphaOp2 = 1;    // D3DTOP_DISABLE
                            uniform.alphaArg1_2 = 2;
                            uniform.alphaArg2_2 = 1;
                            ++metrics.bindingOverflows;
                        }
                        uniform.bumpEnvMat0_00 = bumpEnv[0][0];
                        uniform.bumpEnvMat0_01 = bumpEnv[0][1];
                        uniform.bumpEnvMat0_10 = bumpEnv[0][2];
                        uniform.bumpEnvMat0_11 = bumpEnv[0][3];
                        uniform.bumpEnvLScale0 = bumpEnv[0][4];
                        uniform.bumpEnvLOffset0 = bumpEnv[0][5];
                        uniform.bumpEnvMat1_00 = bumpEnv[1][0];
                        uniform.bumpEnvMat1_01 = bumpEnv[1][1];
                        uniform.bumpEnvMat1_10 = bumpEnv[1][2];
                        uniform.bumpEnvMat1_11 = bumpEnv[1][3];
                        uniform.bumpEnvLScale1 = bumpEnv[1][4];
                        uniform.bumpEnvLOffset1 = bumpEnv[1][5];
                        // ponytail: log the actual bump-op/matrix a draw is using
                        // every couple seconds (not just once ever - the first
                        // match tends to be a startup draw before textures are
                        // uploaded, all falling back to the white slot 0), and
                        // only once textures actually resolved, so this reflects
                        // a real steady-state water draw.
                        if (alphaBlendEnabled) {
                            static NSTimeInterval lastLogged = 0;
                            const NSTimeInterval now = CACurrentMediaTime();
                            if (now - lastLogged > 1.0) {
                                lastLogged = now;
                                NSLog(@"DIAG blend draw: colorOp0=%u alphaOp0=%u colorArg1_0=%u "
                                      "colorArg2_0=%u alphaArg1_0=%u alphaArg2_0=%u tex0=%u "
                                      "colorOp1=%u tex1=%u "
                                      "colorOp2=%u alphaOp2=%u colorArg1_2=%u colorArg2_2=%u "
                                      "alphaArg1_2=%u alphaArg2_2=%u tex2=%u "
                                      "mat0=(%.3f %.3f %.3f %.3f) mat1=(%.3f %.3f %.3f %.3f) "
                                      "lscale1=%.3f loffset1=%.3f alphaBlendEnabled=%d "
                                      "srcBlend=%u destBlend=%u zWrite=%d",
                                      uniform.colorOp, uniform.alphaOp,
                                      uniform.colorArg1, uniform.colorArg2,
                                      uniform.alphaArg1, uniform.alphaArg2,
                                      uniform.textureIndex,
                                      uniform.colorOp1, uniform.textureIndex1,
                                      uniform.colorOp2, uniform.alphaOp2,
                                      uniform.colorArg1_2, uniform.colorArg2_2,
                                      uniform.alphaArg1_2, uniform.alphaArg2_2,
                                      uniform.textureIndex2,
                                      uniform.bumpEnvMat0_00, uniform.bumpEnvMat0_01,
                                      uniform.bumpEnvMat0_10, uniform.bumpEnvMat0_11,
                                      uniform.bumpEnvMat1_00, uniform.bumpEnvMat1_01,
                                      uniform.bumpEnvMat1_10, uniform.bumpEnvMat1_11,
                                      uniform.bumpEnvLScale1, uniform.bumpEnvLOffset1,
                                      alphaBlendEnabled, sourceBlend, destinationBlend,
                                      zWriteEnabled);
                            }
                        }
                        const NSUInteger vertexType = draw.fvf == DK2M_FVF_VERTEX1C ? 0 : 1;
                        id<MTLRenderPipelineState> pipeline = _opaquePipelines[vertexType];
                        if (alphaBlendEnabled) {
                            pipeline = sourceBlend == 2 && destinationBlend == 2
                                           ? _additivePipelines[vertexType]
                                           : _alphaPipelines[vertexType];
                        }
                        [encoder setRenderPipelineState:pipeline];
                        const uint32_t effectiveZFunction = zEnabled ? zFunction : 8;
                        const uint32_t effectiveZWrite = zEnabled && zWriteEnabled ? 1 : 0;
                        [encoder setDepthStencilState:_depthStates[effectiveZFunction][effectiveZWrite]];
                        [encoder setCullMode:cullMode == 1 ? MTLCullModeNone : MTLCullModeBack];
                        [encoder setFrontFacingWinding:cullMode == 3 ? MTLWindingClockwise
                                                                    : MTLWindingCounterClockwise];
                        if (boundArgumentTableBank != currentTextureBinding.bank) {
                            boundArgumentTableBank = currentTextureBinding.bank;
                            [encoder setArgumentTable:
                                _argumentTables[slot][boundArgumentTableBank]
                                             atStages:MTLRenderStageVertex |
                                                      MTLRenderStageFragment];
                        }
                        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                            indexCount:draw.index_count
                                             indexType:MTLIndexTypeUInt16
                                           indexBuffer:_indexBuffers[slot].gpuAddress + indexOffset
                                     indexBufferLength:indexBytes
                                         instanceCount:1
                                            baseVertex:alignedVertexOffset / bridgeVertexSize
                                          baseInstance:drawUniformCount];
                        vertexOffset = alignedVertexOffset + vertexBytes;
                        indexOffset += indexBytes;
                        ++drawUniformCount;
                    } else {
                        ++metrics.invalidDraws;
                    }
                }
            }
            }
            gBridgeFramesRendered.fetch_add(1, std::memory_order_relaxed);
        }
        [encoder endEncoding];
        if (bloomActive) {
            [self encodeBloomIntoCommandBuffer:commandBuffer
                                           slot:slot
                                drawableTexture:update.drawable.texture];
        }
        [commandBuffer endCommandBuffer];

        const auto drawableWaitStarted = TelemetryClock::now();
        [_queue waitForDrawable:update.drawable];
        metrics.drawableWaitUs = elapsedMicroseconds(drawableWaitStarted,
                                                      TelemetryClock::now());
        id<MTL4CommandBuffer> submissions[] = {commandBuffer};
        [_queue commit:submissions count:1];
        [_queue signalEvent:_completed value:_frame + 1];
        [_queue signalDrawable:update.drawable];
        [update.drawable present];
        _submittedAt[slot] = TelemetryClock::now();
        _submittedValue[slot] = _frame + 1;

        ++_frame;
        if (snapshot) _lastBridgeFrame = snapshot->frame;
        if (snapshot && snapshot->width && snapshot->height) {
            const uint64_t packed = packSize(CGSizeMake(snapshot->width, snapshot->height));
            if (gGameFrameSize.exchange(packed, std::memory_order_acq_rel) != packed) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [NSApp.windows.firstObject.contentView setNeedsLayout:YES];
                });
            }
        }
        gRenderedFrames.store(_frame, std::memory_order_release);
        if (newBridgeFrame) {
            metrics.encodeUs = elapsedMicroseconds(encodeStarted, TelemetryClock::now());
            _telemetry.add(metrics);
        }

        const uint64_t requested = gRequestedDrawableSize.load(std::memory_order_acquire);
        if (requested != 0 && requested != _appliedDrawableSize) {
            _layer.drawableSize = unpackSize(requested);
            _appliedDrawableSize = requested;
        }

        const bool bridgeReady = !gBridgeRequired || gBridgeFramesRendered.load(std::memory_order_acquire) != 0;
        if (gSelfTestFrames != 0 && _frame >= gSelfTestFrames && bridgeReady) {
            link.paused = YES;
            NSLog(@"SELF-TEST PASS: %llu validated Metal 4 frames", _frame);
            dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
        }
    }
}

@end

// Native AppKit Preferences window (Cmd-,): the only editor for settings.toml
// besides hand-editing the file. Every control saves immediately (single
// writer func, atomic replace) -- there is no separate "Apply"/"OK", matching
// the file being the primary source of truth. [renderer] controls apply live;
// everything else is marked "restart required" and only takes effect the
// next time the game (not the host) launches.
@interface DK2PreferencesWindowController : NSWindowController <NSTextFieldDelegate>
+ (instancetype)shared;
@end

@implementation DK2PreferencesWindowController {
    NSButton *_bloom;
    NSButton *_metalShadows;
    NSButton *_hdTextures;
    NSButton *_movies;
    NSButton *_meshGpuPath;
    NSButton *_shadowCache;
    NSButton *_debugProbes;
    NSSlider *_renderScale;
    NSTextField *_renderScaleLabel;
    NSPopUpButton *_shadowLevel;
    NSComboBox *_resolution;
    NSTextField *_level;
    NSTextField *_levelWarning;
    NSTextField *_winedebug;
}

+ (instancetype)shared {
    static DK2PreferencesWindowController *instance;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ instance = [[DK2PreferencesWindowController alloc] init]; });
    return instance;
}

- (instancetype)init {
    NSRect frame = NSMakeRect(0, 0, 560, 520);
    NSWindow *window = [[NSWindow alloc] initWithContentRect:frame
                                                    styleMask:NSWindowStyleMaskTitled |
                                                              NSWindowStyleMaskClosable |
                                                              NSWindowStyleMaskResizable
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
    window.title = @"Dungeon Keeper II Preferences";
    window.releasedWhenClosed = NO;
    // Content (4 section headers + 12 rows) is taller than any reasonable
    // fixed window, so this is a floor, not the real size -- see buildUI's
    // NSScrollView.
    window.minSize = NSMakeSize(560, 280);
    self = [super initWithWindow:window];
    if (self) [self buildUI];
    return self;
}

- (NSButton *)checkboxWithFrame:(NSRect)frame title:(NSString *)title checked:(BOOL)checked {
    NSButton *box = [[NSButton alloc] initWithFrame:frame];
    box.buttonType = NSButtonTypeSwitch;
    box.title = title;
    box.state = checked ? NSControlStateValueOn : NSControlStateValueOff;
    box.target = self;
    box.action = @selector(controlChanged:);
    return box;
}

- (NSTextField *)labelWithFrame:(NSRect)frame text:(NSString *)text bold:(BOOL)bold {
    NSTextField *field = [NSTextField labelWithString:text];
    field.frame = frame;
    if (bold) field.font = [NSFont boldSystemFontOfSize:13];
    return field;
}

- (NSString *)percentString:(float)scale {
    return [NSString stringWithFormat:@"%d%%", (int)std::lround(scale * 100.0f)];
}

- (void)buildUI {
    dk2cfg::Settings s;
    {
        std::lock_guard<std::mutex> lock(dk2cfg::gMutex);
        s = dk2cfg::gSettings;
    }

    // The content (4 section headers + 12 rows) is reliably taller than any
    // reasonable fixed window -- this was previously sized to the window's
    // own bounds, which silently clipped the bottom rows (Shadow Cache,
    // Debug Probes, WineDebug) with no way to reach them. Compute the real
    // content height up front and put it in a scroll view instead.
    const CGFloat labelX = 20, labelWidth = 150;
    const CGFloat controlX = 180, controlWidth = 220;
    const CGFloat hintX = 410, hintWidth = 130;
    const CGFloat rowHeight = 24, rowGap = 10, headerGap = 30;
    const CGFloat topMargin = 40, bottomMargin = 20;
    const int headerCount = 4;   // Renderer, Game, Patches, Developer
    const int rowCount = 14;     // 4 + 3 + 3 + (caption + level + warning + winedebug)
    const CGFloat contentWidth = 560;
    const CGFloat contentHeight = topMargin + bottomMargin +
        headerCount * (headerGap + rowHeight + rowGap) +
        rowCount * (rowHeight + rowGap);

    NSView *container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, contentWidth, contentHeight)];
    CGFloat y = contentHeight - topMargin;

    auto addHeader = [&](NSString *title) {
        y -= headerGap;
        [container addSubview:[self labelWithFrame:NSMakeRect(labelX, y, 400, 20)
                                               text:title bold:YES]];
        y -= rowHeight + rowGap;
    };
    auto addHint = [&](CGFloat rowY) {
        NSTextField *hint = [self labelWithFrame:NSMakeRect(hintX, rowY, hintWidth, rowHeight)
                                             text:@"restart required" bold:NO];
        hint.font = [NSFont systemFontOfSize:10];
        hint.textColor = NSColor.secondaryLabelColor;
        [container addSubview:hint];
    };
    auto addRowLabel = [&](NSString *title) {
        [container addSubview:[self labelWithFrame:NSMakeRect(labelX, y, labelWidth, rowHeight)
                                               text:title bold:NO]];
    };
    auto addCaption = [&](NSString *text) {
        NSTextField *caption = [self labelWithFrame:NSMakeRect(labelX, y, hintX + hintWidth - labelX, rowHeight)
                                                text:text bold:NO];
        caption.font = [NSFont systemFontOfSize:11];
        caption.textColor = NSColor.secondaryLabelColor;
        [container addSubview:caption];
        y -= rowHeight + rowGap;
    };

    addHeader(@"Renderer (applies immediately)");
    addRowLabel(@"Bloom");
    _bloom = [self checkboxWithFrame:NSMakeRect(controlX, y, controlWidth, rowHeight)
                                title:@"" checked:s.bloom];
    [container addSubview:_bloom];
    y -= rowHeight + rowGap;

    addRowLabel(@"Metal Shadows");
    _metalShadows = [self checkboxWithFrame:NSMakeRect(controlX, y, controlWidth, rowHeight)
                                       title:@"" checked:s.metalShadows];
    [container addSubview:_metalShadows];
    y -= rowHeight + rowGap;

    addRowLabel(@"HD Textures");
    _hdTextures = [self checkboxWithFrame:NSMakeRect(controlX, y, controlWidth, rowHeight)
                                     title:@"" checked:s.hdTextures];
    [container addSubview:_hdTextures];
    y -= rowHeight + rowGap;

    addRowLabel(@"Render Scale");
    _renderScale = [NSSlider sliderWithValue:s.renderScale
                                     minValue:0.375
                                     maxValue:1.0
                                       target:self
                                       action:@selector(controlChanged:)];
    _renderScale.frame = NSMakeRect(controlX, y, controlWidth - 56, rowHeight);
    _renderScale.continuous = YES;
    [container addSubview:_renderScale];
    _renderScaleLabel = [self labelWithFrame:NSMakeRect(controlX + controlWidth - 48, y, 48, rowHeight)
                                        text:[self percentString:s.renderScale] bold:NO];
    [container addSubview:_renderScaleLabel];
    y -= rowHeight + rowGap;

    addHeader(@"Game");
    addRowLabel(@"Shadow Level");
    _shadowLevel = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(controlX, y, 80, rowHeight)
                                               pullsDown:NO];
    for (int i = 0; i <= 3; ++i) [_shadowLevel addItemWithTitle:[NSString stringWithFormat:@"%d", i]];
    [_shadowLevel selectItemAtIndex:std::clamp(s.shadowLevel, 0, 3)];
    _shadowLevel.target = self;
    _shadowLevel.action = @selector(controlChanged:);
    [container addSubview:_shadowLevel];
    addHint(y);
    y -= rowHeight + rowGap;

    addRowLabel(@"Resolution");
    _resolution = [[NSComboBox alloc] initWithFrame:NSMakeRect(controlX, y, controlWidth, rowHeight)];
    [_resolution addItemsWithObjectValues:@[@"1600x1200", @"1800x1200 (experimental, HUD gaps)"]];
    _resolution.stringValue = @(s.resolution.c_str());
    _resolution.target = self;
    _resolution.action = @selector(controlChanged:);
    [container addSubview:_resolution];
    addHint(y);
    y -= rowHeight + rowGap;

    addRowLabel(@"Movies");
    _movies = [self checkboxWithFrame:NSMakeRect(controlX, y, controlWidth, rowHeight)
                                 title:@"" checked:s.movies];
    [container addSubview:_movies];
    addHint(y);
    y -= rowHeight + rowGap;

    addHeader(@"Patches");
    addRowLabel(@"Mesh GPU Path");
    _meshGpuPath = [self checkboxWithFrame:NSMakeRect(controlX, y, controlWidth, rowHeight)
                                      title:@"" checked:s.meshGpuPath];
    [container addSubview:_meshGpuPath];
    addHint(y);
    y -= rowHeight + rowGap;

    addRowLabel(@"Shadow Cache");
    _shadowCache = [self checkboxWithFrame:NSMakeRect(controlX, y, controlWidth, rowHeight)
                                      title:@"" checked:s.shadowCache];
    [container addSubview:_shadowCache];
    addHint(y);
    y -= rowHeight + rowGap;

    addRowLabel(@"Debug Probes");
    _debugProbes = [self checkboxWithFrame:NSMakeRect(controlX, y, controlWidth, rowHeight)
                                      title:@"" checked:s.debugProbes];
    [container addSubview:_debugProbes];
    addHint(y);
    y -= rowHeight + rowGap;

    // Separated visually from the everyday Game/Patches settings above: both
    // fields here are development/debugging aids, not something a player
    // adjusts for normal play, and level in particular bypasses the main
    // menu entirely on the next launch -- surprising if it sits next to
    // Movies/Resolution looking like just another game option.
    addHeader(@"Developer");
    addCaption(@"Level autostart — skips the main menu; leave empty for normal play.");

    addRowLabel(@"Level");
    _level = [[NSTextField alloc] initWithFrame:NSMakeRect(controlX, y, controlWidth, rowHeight)];
    _level.stringValue = @(s.level.c_str());
    _level.placeholderString = @"empty = normal menu";
    _level.delegate = self;
    _level.target = self;
    _level.action = @selector(controlChanged:);
    [container addSubview:_level];
    addHint(y);
    y -= rowHeight + rowGap;

    _levelWarning = [[NSTextField alloc] initWithFrame:NSMakeRect(labelX, y, hintX + hintWidth - labelX, rowHeight)];
    _levelWarning.editable = NO;
    _levelWarning.selectable = NO;
    _levelWarning.bordered = NO;
    _levelWarning.drawsBackground = YES;
    _levelWarning.backgroundColor = [NSColor colorWithCalibratedRed:1.0 green:0.85 blue:0.35 alpha:1.0];
    _levelWarning.textColor = NSColor.blackColor;
    _levelWarning.font = [NSFont boldSystemFontOfSize:11];
    _levelWarning.alignment = NSTextAlignmentCenter;
    _levelWarning.stringValue = @"Menu will be skipped on next launch";
    _levelWarning.hidden = s.level.empty();
    [container addSubview:_levelWarning];
    y -= rowHeight + rowGap;

    addRowLabel(@"WineDebug");
    _winedebug = [[NSTextField alloc] initWithFrame:NSMakeRect(controlX, y, controlWidth, rowHeight)];
    _winedebug.stringValue = @(s.winedebug.c_str());
    _winedebug.delegate = self;
    _winedebug.target = self;
    _winedebug.action = @selector(controlChanged:);
    [container addSubview:_winedebug];
    addHint(y);
    y -= rowHeight + rowGap;

    NSScrollView *scrollView = [[NSScrollView alloc] initWithFrame:self.window.contentView.bounds];
    scrollView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scrollView.hasVerticalScroller = YES;
    scrollView.hasHorizontalScroller = NO;
    scrollView.autohidesScrollers = YES;
    scrollView.drawsBackground = NO;
    scrollView.documentView = container;
    self.window.contentView = scrollView;
}

// Reads every control's current value (not just sender's) and saves the
// whole settings.toml in one atomic write.
- (void)controlChanged:(id)sender {
    (void)sender;
    dk2cfg::Settings s;
    s.shadowLevel = (int)_shadowLevel.indexOfSelectedItem;
    std::string resolution = _resolution.stringValue.UTF8String;
    const size_t paren = resolution.find(" (");
    if (paren != std::string::npos) resolution = resolution.substr(0, paren);
    s.resolution = resolution;
    s.movies = _movies.state == NSControlStateValueOn;
    s.level = _level.stringValue.UTF8String;
    s.bloom = _bloom.state == NSControlStateValueOn;
    s.metalShadows = _metalShadows.state == NSControlStateValueOn;
    s.renderScale = std::clamp((float)_renderScale.doubleValue, 0.375f, 1.0f);
    s.hdTextures = _hdTextures.state == NSControlStateValueOn;
    s.meshGpuPath = _meshGpuPath.state == NSControlStateValueOn;
    s.shadowCache = _shadowCache.state == NSControlStateValueOn;
    s.debugProbes = _debugProbes.state == NSControlStateValueOn;
    s.winedebug = _winedebug.stringValue.UTF8String;

    _renderScaleLabel.stringValue = [self percentString:s.renderScale];
    _levelWarning.hidden = s.level.empty();

    if (!dk2cfg::writeAtomic(s)) {
        NSLog(@"WARNING: failed to save settings.toml");
    }
    // Apply immediately rather than waiting on the file watcher's round trip
    // through the filesystem; the watcher firing right after is a harmless
    // no-op re-application of the same values.
    dk2cfg::applyRendererLive(s);
}

// NSTextField sends -action on Return and (being non-continuous, the default
// we rely on here) does NOT send it per keystroke -- but that alone still
// leaves "click away without pressing Return" as a way to lose an edit
// silently. Catching -controlTextDidEndEditing: (fired on any end-of-editing:
// Return, Tab, or the field simply resigning first responder) makes commit
// unambiguous: Enter or focus-loss, never per-keystroke, matching what the
// coordinator asked to verify.
- (void)controlTextDidEndEditing:(NSNotification *)notification {
    [self controlChanged:notification.object];
}

@end

@interface DK2AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation DK2AppDelegate {
    NSWindow *_window;
    DK2MetalView *_view;
    DK2MetalRenderer *_renderer;
    NSTask *_gameRunner;
    BOOL _terminating;
}

- (void)startBundledGameRunner {
    // Pick the game resolution to match this display's aspect (height stays
    // 1200, the largest the engine's font/UI tables are tuned for). The
    // bridge-side mode check accepts any size, so widescreen just works.
    // ponytail: clamp 1600..2560, revisit if ultrawide displays misbehave.
    // Default stays 4:3 1600x1200: the in-game HUD lays out panel
    // backgrounds for 4:3 widths, so wide game resolutions show black gaps
    // beside the bottom toolbar. Widescreen (e.g. DK2_GAME_RES=1800x1200)
    // remains an explicit override until the HUD layout is widescreen-aware.
    if (gGameRunnerPath) {
        // Dev flow: keep the Wine runner tied to the native app lifecycle.
        _gameRunner = [[NSTask alloc] init];
        _gameRunner.executableURL = [NSURL fileURLWithPath:gGameRunnerPath];
        __weak DK2AppDelegate *weakSelf = self;
        _gameRunner.terminationHandler = ^(NSTask *task) {
            (void)task;
            dispatch_async(dispatch_get_main_queue(), ^{
                DK2AppDelegate *delegate = weakSelf;
                if (delegate && !delegate->_terminating) [NSApp terminate:nil];
            });
        };
        NSError *runnerError = nil;
        if (![_gameRunner launchAndReturnError:&runnerError]) {
            fail([NSString stringWithFormat:@"Unable to start the game runner: %@",
                                            runnerError.localizedDescription]);
        }
        return;
    }
    if (gBridgePath) return;
    NSURL *runner = [NSBundle.mainBundle URLForResource:@"dk2-game-runner" withExtension:nil];
    if (!runner) return;

    NSError *error = nil;
    NSURL *support = [NSFileManager.defaultManager URLForDirectory:NSApplicationSupportDirectory
                                                          inDomain:NSUserDomainMask
                                                 appropriateForURL:nil
                                                            create:YES
                                                             error:&error];
    NSURL *bridgeDirectory = [[support URLByAppendingPathComponent:@"Dungeon Keeper II"
                                                        isDirectory:YES]
        URLByAppendingPathComponent:@"prefix/drive_c/dk2-metal" isDirectory:YES];
    if (!support || ![NSFileManager.defaultManager createDirectoryAtURL:bridgeDirectory
                                            withIntermediateDirectories:YES
                                                             attributes:nil
                                                                  error:&error]) {
        fail([NSString stringWithFormat:@"Unable to create application data: %@", error.localizedDescription]);
        return;
    }
    gBridgePath = [[bridgeDirectory URLByAppendingPathComponent:@"frame.bin"] path];

    _gameRunner = [[NSTask alloc] init];
    _gameRunner.executableURL = runner;
    _gameRunner.currentDirectoryURL = NSBundle.mainBundle.resourceURL;
    __weak DK2AppDelegate *weakSelf = self;
    _gameRunner.terminationHandler = ^(NSTask *task) {
        (void)task;
        dispatch_async(dispatch_get_main_queue(), ^{
            DK2AppDelegate *delegate = weakSelf;
            if (delegate && !delegate->_terminating) [NSApp terminate:nil];
        });
    };
    if (![_gameRunner launchAndReturnError:&error]) {
        fail([NSString stringWithFormat:@"Unable to start the game: %@", error.localizedDescription]);
    }
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    [self installMenu];

    NSRect content = NSMakeRect(0, 0, 1280, 960);
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    _window = [[NSWindow alloc] initWithContentRect:content
                                          styleMask:style
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = @"Dungeon Keeper II — Native Metal";
    _window.delegate = self;
    _window.minSize = NSMakeSize(640, 480);
    _window.contentAspectRatio = NSMakeSize(4, 3);
    _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    _window.tabbingMode = NSWindowTabbingModeDisallowed;

    _view = [[DK2MetalView alloc] initWithFrame:content];
    _window.contentView = _view;
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_view];
    [NSApp activateIgnoringOtherApps:YES];
    [_view setInputActive:YES];

    gMetalView = _view;
    dk2cfg::gOnApplied = [] {
        // render_scale changed; re-derive the drawable size without waiting
        // for an unrelated window resize (bloom/shadows/hd_textures need no
        // extra nudge, they are read fresh every frame/texture-upload).
        if (gMetalView) dispatch_async(dispatch_get_main_queue(), ^{ [gMetalView setNeedsLayout:YES]; });
    };
    dk2cfg::startWatching();

    [self startBundledGameRunner];

    [_view layoutSubtreeIfNeeded];
    CAMetalLayer *layer = _view.metalLayer;
    const uint64_t requestedSize = gRequestedDrawableSize.load(std::memory_order_acquire);
    const CGSize drawableSize = requestedSize ? unpackSize(requestedSize) : CGSizeMake(2560, 1920);
    layer.drawableSize = drawableSize;
    gRequestedDrawableSize.store(packSize(drawableSize), std::memory_order_release);

    _renderer = [[DK2MetalRenderer alloc] initWithLayer:layer];
    [_view publishCurrentInput];
    [_renderer start];
    if (gStartFullscreen && !(_window.styleMask & NSWindowStyleMaskFullScreen)) {
        // Native full-screen Space; Info.plist explicitly opts out of Game Mode.
        [_window toggleFullScreen:nil];
    }
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    _terminating = YES;
    _gameRunner.terminationHandler = nil;
    if (_gameRunner.running) [_gameRunner terminate];
}

- (void)windowDidBecomeKey:(NSNotification *)notification {
    (void)notification;
    [_window makeFirstResponder:_view];
    [_view setInputActive:YES];
}

- (void)windowDidResignKey:(NSNotification *)notification {
    (void)notification;
    [_view setInputActive:NO];
}

- (void)installMenu {
    NSMenu *bar = [[NSMenu alloc] init];
    NSMenuItem *applicationItem = [[NSMenuItem alloc] init];
    [bar addItem:applicationItem];
    NSMenu *applicationMenu = [[NSMenu alloc] init];
    [applicationMenu addItemWithTitle:@"Settings…"
                               action:@selector(openPreferences:)
                        keyEquivalent:@","];
    [applicationMenu addItem:[NSMenuItem separatorItem]];
    [applicationMenu addItemWithTitle:@"Quit Dungeon Keeper II"
                               action:@selector(terminate:)
                        keyEquivalent:@"q"];
    applicationItem.submenu = applicationMenu;

    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    [bar addItem:viewItem];
    NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    [viewMenu addItemWithTitle:@"Enter Full Screen"
                        action:@selector(toggleFullScreen:)
                 keyEquivalent:@"f"].keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
    viewItem.submenu = viewMenu;
    NSApp.mainMenu = bar;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return YES;
}

- (void)openPreferences:(id)sender {
    (void)sender;
    [NSApp activateIgnoringOtherApps:YES];
    [[DK2PreferencesWindowController shared] showWindow:nil];
    [[[DK2PreferencesWindowController shared] window] makeKeyAndOrderFront:nil];
}

@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        for (int index = 1; index < argc; ++index) {
            NSString *argument = [NSString stringWithUTF8String:argv[index]];
            if ([argument hasPrefix:@"--self-test-frames="]) {
                gSelfTestFrames = [[argument substringFromIndex:19] longLongValue];
            } else if ([argument hasPrefix:@"--bridge-file="]) {
                gBridgePath = [argument substringFromIndex:14];
            } else if ([argument isEqualToString:@"--bridge-self-test"]) {
                gBridgeRequired = true;
            } else if ([argument isEqualToString:@"--fullscreen"]) {
                gStartFullscreen = true;
            } else if ([argument hasPrefix:@"--game-runner="]) {
                gGameRunnerPath = [argument substringFromIndex:14];
            } else if ([argument hasPrefix:@"--runner-env="]) {
                // K=V exported to the runner and this process so the child
                // starts with the caller's configuration intact.
                NSString *pair = [argument substringFromIndex:13];
                const NSRange eq = [pair rangeOfString:@"="];
                if (eq.location != NSNotFound && eq.location > 0) {
                    setenv([pair substringToIndex:eq.location].UTF8String,
                           [pair substringFromIndex:eq.location + 1].UTF8String, 1);
                }
            }
        }

        // settings.toml is the base; anything already in the environment at
        // this point (inherited from the parent shell, or just set above via
        // --runner-env=) wins over it on every key -- see dk2cfg::setenvIfUnset.
        dk2cfg::initEnvOverrides();
        {
            const dk2cfg::Settings settings = dk2cfg::load();
            dk2cfg::gLaunchSettings = settings;
            dk2cfg::applyRendererLive(settings);
            dk2cfg::composeRunnerEnv(settings);
        }

        // Single-instance guard: a second launch against the same prefix
        // kills the first one's wineserver and both hosts fight over the
        // bridge file - the user just sees a white window. Refuse instead.
        if (gSelfTestFrames == 0) {
            NSString *lockPath = [NSString stringWithFormat:@"%@/dk2metal.lock", NSTemporaryDirectory()];
            const int lockFd = open(lockPath.fileSystemRepresentation, O_CREAT | O_RDWR, 0644);
            if (lockFd >= 0 && flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
                NSAlert *alert = [[NSAlert alloc] init];
                alert.messageText = @"Dungeon Keeper II is already running";
                alert.informativeText = @"Close the running copy before starting a new one.";
                [NSApplication.sharedApplication setActivationPolicy:NSApplicationActivationPolicyRegular];
                [alert runModal];
                return 1;
            }
            // lockFd intentionally stays open for the process lifetime
        }

        NSApplication *application = NSApplication.sharedApplication;
        application.activationPolicy = NSApplicationActivationPolicyRegular;
        DK2AppDelegate *delegate = [[DK2AppDelegate alloc] init];
        application.delegate = delegate;
        [application run];
        const bool frameFailure = gSelfTestFrames != 0 && gRenderedFrames.load() < gSelfTestFrames;
        const bool bridgeFailure = gBridgeRequired && gBridgeFramesRendered.load() == 0;
        return frameFailure || bridgeFailure ? 1 : 0;
    }
}
