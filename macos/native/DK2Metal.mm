#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "metal_bridge/DK2BridgeProtocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#import <ImageIO/ImageIO.h>

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
                          @"Library/Application Support/Dungeon Keeper 2 Flame/texture-dump"];
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

NSString *directory() {
    static NSString *dir = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *env = std::getenv("DK2_TEXTURE_HD");
        NSString *path = (env && *env)
                ? @(env)
                : [NSHomeDirectory() stringByAppendingPathComponent:
                          @"Library/Application Support/Dungeon Keeper 2 Flame/textures-hd"];
        BOOL isDir = NO;
        if ([[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&isDir] && isDir) {
            dir = path;
        }
    });
    return dir;
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
    static std::unordered_map<uint64_t, id<MTLTexture>> loaded;
    static std::unordered_set<uint64_t> missing;
    const uint64_t hash = texdump::contentHash(pixels, width, height, pitch);
    auto found = loaded.find(hash);
    if (found != loaded.end()) return found->second;
    if (missing.count(hash)) return nil;
    NSString *path = [dir stringByAppendingPathComponent:
            [NSString stringWithFormat:@"%016llx.png", hash]];
    id<MTLTexture> texture = access(path.fileSystemRepresentation, R_OK) == 0
            ? loadFile(device, path, hash)
            : nil;
    if (texture) {
        loaded.emplace(hash, texture);
        NSLog(@"texhd: %016llx replaced with %lux%lu",
              hash, (unsigned long)texture.width, (unsigned long)texture.height);
    } else {
        if (missing.size() > 100000) missing.clear();  // dynamic frames grow this
        missing.insert(hash);
    }
    return texture;
}

}  // namespace texhd

namespace {

constexpr NSUInteger kFramesInFlight = 3;
constexpr NSUInteger kSampleCount = 4;
std::atomic<uint64_t> gRequestedDrawableSize{0};
std::atomic<uint64_t> gRenderedFrames{0};
std::atomic<uint64_t> gBridgeFramesRendered{0};
uint64_t gSelfTestFrames = 0;
NSString *gBridgePath = nil;
NSString *gGameRunnerPath = nil;
bool gStartFullscreen = false;
bool gBridgeRequired = false;
std::atomic<DK2MFileHeader *> gInputHeader{nullptr};

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
               "fvf1=%llu fvf2=%llu missing-texture=%llu binding-overflow=%llu invalid-draw=%llu",
              p(&FrameMetrics::encodeUs, 50), p(&FrameMetrics::encodeUs, 95),
              p(&FrameMetrics::encodeUs, 99), p(&FrameMetrics::drawableWaitUs, 50),
              p(&FrameMetrics::drawableWaitUs, 95), p(&FrameMetrics::drawableWaitUs, 99),
              p(&FrameMetrics::gpuWaitUs, 50), p(&FrameMetrics::gpuWaitUs, 95),
              p(&FrameMetrics::gpuWaitUs, 99), p(&FrameMetrics::gpuCompleteUs, 50),
              p(&FrameMetrics::gpuCompleteUs, 95), p(&FrameMetrics::gpuCompleteUs, 99),
              total(&FrameMetrics::fvf1Draws), total(&FrameMetrics::fvf2Draws),
              total(&FrameMetrics::missingTextures), total(&FrameMetrics::bindingOverflows),
              total(&FrameMetrics::invalidDraws));
        NSLog(@"PERF producer us p50/p95/p99: draw-copy=%u/%u/%u texture=%u/%u/%u "
               "overlay=%u/%u/%u",
              p(&FrameMetrics::producerDrawCopyUs, 50), p(&FrameMetrics::producerDrawCopyUs, 95),
              p(&FrameMetrics::producerDrawCopyUs, 99), p(&FrameMetrics::producerTextureUs, 50),
              p(&FrameMetrics::producerTextureUs, 95), p(&FrameMetrics::producerTextureUs, 99),
              p(&FrameMetrics::producerOverlayUs, 50), p(&FrameMetrics::producerOverlayUs, 95),
              p(&FrameMetrics::producerOverlayUs, 99));
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
constexpr NSUInteger kTextureArgumentTablesPerFrame = 16;
constexpr uint32_t kD3DRenderStateZEnable = 7;
constexpr uint32_t kD3DRenderStateZWriteEnable = 14;
constexpr uint32_t kD3DRenderStateSourceBlend = 19;
constexpr uint32_t kD3DRenderStateDestinationBlend = 20;
constexpr uint32_t kD3DRenderStateCullMode = 22;
constexpr uint32_t kD3DRenderStateZFunc = 23;
constexpr uint32_t kD3DRenderStateAlphaBlendEnable = 27;
constexpr uint32_t kD3DRenderStateTextureFactor = 60;
static_assert(sizeof(DrawUniform) == 68);

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
    publishInputState(_input);
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
    const CGFloat targetAspect = 4.0 / 3.0;
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
    gRequestedDrawableSize.store(
        packSize(CGSizeMake(backingSize.width, backingSize.height)),
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
    id<MTLTexture> _whiteTexture;
    id<MTLTexture> _multisampleColorTexture;
    id<MTLTexture> _depthTexture;
    std::unordered_map<uint32_t, id<MTLTexture>> _textures;
    id<MTLResidencySet> _resources;
    id<MTL4CommandAllocator> _allocators[kFramesInFlight];
    id<MTL4CommandBuffer> _commandBuffers[kFramesInFlight];
    id<MTLBuffer> _vertexBuffers[kFramesInFlight];
    id<MTLBuffer> _indexBuffers[kFramesInFlight];
    id<MTLBuffer> _drawBuffers[kFramesInFlight];
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
        uint8_t ringIndex = 0;
        bool dynamic = false;
        id<MTLTexture> ring[kFramesInFlight] = {};
    };
    std::unordered_map<uint32_t, DynamicTexture> _dynamicTextures;
    std::vector<CommandView> _commandViews;
    std::vector<TextureBindingEntry> _frameTextureBindings;
    TelemetryWindow _telemetry;
    TelemetryClock::time_point _lastBridgeArrival;
    TelemetryClock::time_point _submittedAt[kFramesInFlight];
    uint64_t _submittedValue[kFramesInFlight];
    uint64_t _frame;
    uint64_t _appliedDrawableSize;
    uint32_t _lastBridgeFrame;
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

        if (!_vertexBuffers[index] || !_indexBuffers[index] || !_drawBuffers[index]) {
            fail(@"Metal dynamic frame buffer creation failed.");
            return nil;
        }
        for (NSUInteger bank = 0; bank < kTextureArgumentTablesPerFrame; ++bank) {
            MTL4ArgumentTableDescriptor *tableDescriptor =
                [[MTL4ArgumentTableDescriptor alloc] init];
            tableDescriptor.maxBufferBindCount = 2;
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
    }

    id<MTLAllocation> allocations[kFramesInFlight * 3 + 1];
    for (NSUInteger index = 0; index < kFramesInFlight; ++index) {
        allocations[index * 3] = _vertexBuffers[index];
        allocations[index * 3 + 1] = _indexBuffers[index];
        allocations[index * 3 + 2] = _drawBuffers[index];
    }
    allocations[kFramesInFlight * 3] = _whiteTexture;
    [_resources addAllocations:allocations count:kFramesInFlight * 3 + 1];
    [_resources commit];
    [_resources requestResidency];
    [_queue addResidencySet:_resources];

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
        if (_multisampleColorTexture) [_resources removeAllocation:_multisampleColorTexture];
        if (_depthTexture) [_resources removeAllocation:_depthTexture];
    }
    MTLTextureDescriptor *colorDescriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:_layer.pixelFormat
                                    width:width height:height mipmapped:NO];
    colorDescriptor.textureType = MTLTextureType2DMultisample;
    colorDescriptor.sampleCount = kSampleCount;
    colorDescriptor.storageMode = MTLStorageModePrivate;
    colorDescriptor.usage = MTLTextureUsageRenderTarget;
    _multisampleColorTexture = [_device newTextureWithDescriptor:colorDescriptor];

    MTLTextureDescriptor *depthDescriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                    width:width height:height mipmapped:NO];
    depthDescriptor.textureType = MTLTextureType2DMultisample;
    depthDescriptor.sampleCount = kSampleCount;
    depthDescriptor.storageMode = MTLStorageModePrivate;
    depthDescriptor.usage = MTLTextureUsageRenderTarget;
    _depthTexture = [_device newTextureWithDescriptor:depthDescriptor];
    if (!_multisampleColorTexture || !_depthTexture) return NO;
    _multisampleColorTexture.label = @"DK2 4x MSAA color";
    _depthTexture.label = @"DK2 4x MSAA depth";
    id<MTLAllocation> targets[] = {_multisampleColorTexture, _depthTexture};
    [_resources addAllocations:targets count:2];
    [_resources commit];
    return YES;
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
        const uint64_t completedValue = _completed.signaledValue;
        for (NSUInteger index = 0; index < kFramesInFlight; ++index) {
            if (_submittedValue[index] && _submittedValue[index] <= completedValue) {
                metrics.gpuCompleteUs = std::max(
                    metrics.gpuCompleteUs,
                    elapsedMicroseconds(_submittedAt[index], encodeStarted));
                _submittedValue[index] = 0;
            }
        }
        const FrameSnapshot *snapshot = _bridge ? _bridge->poll() : nullptr;
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
        BOOL residencyChanged = NO;
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
                        const uint8_t *pixels = snapshot->bytes.data() + offset + sizeof(textureUpdate);
                        DynamicTexture &dyn = _dynamicTextures[textureUpdate.texture_id];
                        id<MTLTexture> hd = nil;
                        const bool cursorTexture =
                            textureUpdate.texture_id == DK2M_CURSOR_TEXTURE_ID;
                        if ((!dyn.dynamic || cursorTexture) &&
                            textureUpdate.texture_id != DK2M_OVERLAY_TEXTURE_ID) {
                            hd = texhd::lookup(_device, pixels, textureUpdate.width,
                                               textureUpdate.height, textureUpdate.row_pitch);
                            if (hd) {
                                dyn.misses = 0;
                            } else if (++dyn.misses > 8 && !cursorTexture) {
                                dyn.dynamic = true;
                                NSLog(@"texture id %u flagged dynamic: hashing and HD lookup off",
                                      textureUpdate.texture_id);
                            }
                        }
                        if ((dyn.dynamic || cursorTexture) && !hd) {
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
                                descriptor.usage = MTLTextureUsageShaderRead;
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
                            if (_textures[key] != hd) {
                                _textures[key] = hd;
                                [_resources addAllocation:hd];
                                residencyChanged = YES;
                            }
                        } else {
                        id<MTLTexture> texture = _textures[key];
                        if (!texture || texture.width != textureUpdate.width ||
                            texture.height != textureUpdate.height) {
                            MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
                                texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                            width:textureUpdate.width
                                                           height:textureUpdate.height
                                                        mipmapped:NO];
                            descriptor.storageMode = MTLStorageModeShared;
                            descriptor.usage = MTLTextureUsageShaderRead;
                            texture = [_device newTextureWithDescriptor:descriptor];
                            if (texture) {
                                texture.label = [NSString stringWithFormat:@"DK2 texture %u", textureUpdate.texture_id];
                                _textures[key] = texture;
                                [_resources addAllocation:texture];
                                residencyChanged = YES;
                            }
                        }
                        if (texture) {
                            [texture replaceRegion:MTLRegionMake2D(0, 0, textureUpdate.width, textureUpdate.height)
                                       mipmapLevel:0 withBytes:pixels bytesPerRow:textureUpdate.row_pitch];
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
                        [texture replaceRegion:MTLRegionMake2D(
                                                   textureUpdate.x, textureUpdate.y,
                                                   textureUpdate.width, textureUpdate.height)
                                   mipmapLevel:0
                                     withBytes:pixels
                                   bytesPerRow:textureUpdate.row_pitch];
                    }
                }
            }
        }
        if (residencyChanged) [_resources commit];
        MTL4RenderPassDescriptor *pass = [[MTL4RenderPassDescriptor alloc] init];
        MTLRenderPassColorAttachmentDescriptor *color = pass.colorAttachments[0];
        color.texture = _multisampleColorTexture;
        color.resolveTexture = update.drawable.texture;
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
        [encoder setDepthStencilState:_depthStates[4][1]];
        if (snapshot) {
            NSUInteger vertexOffset = 0;
            NSUInteger indexOffset = 0;
            NSUInteger drawUniformCount = 0;
            auto *drawUniforms = static_cast<DrawUniform *>(_drawBuffers[slot].contents);
            TextureBinding currentTextureBinding = {0, 0};
            TextureBinding currentTextureBinding1 = {0, 0};
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
                    setTexture:_whiteTexture.gpuResourceID atIndex:0];
                [_argumentTables[slot][bank]
                    setSamplerState:_sampler.gpuResourceID atIndex:0];
            }
            [encoder setArgumentTable:_argumentTables[slot][boundArgumentTableBank]
                             atStages:MTLRenderStageVertex | MTLRenderStageFragment];
            for (const CommandView &view : _commandViews) {
                const size_t commandOffset = view.offset;
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
                    } else {
                        // ponytail: one-shot NSLog per distinct (stage, state) the
                        // combiner doesn't model (e.g. D3DTSS_TEXCOORDINDEX=11,
                        // D3DTSS_BUMPENVMAT*=10/12/13/14, TEXTURETRANSFORMFLAGS=24).
                        // Water/env-mapped draws are the prime suspect for actually
                        // relying on one of these - log first sighting to confirm.
                        static std::unordered_set<uint32_t> seenStageStates;
                        const uint32_t key = (state.stage << 16) | state.state;
                        if (seenStageStates.insert(key).second) {
                            NSLog(@"DIAG unhandled texture-stage-state: stage=%u state=%u value=%u",
                                  state.stage, state.state, state.value);
                        }
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
            gBridgeFramesRendered.fetch_add(1, std::memory_order_relaxed);
        }
        [encoder endEncoding];
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
    NSURL *bridgeDirectory = [[support URLByAppendingPathComponent:@"Dungeon Keeper II Metal"
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
