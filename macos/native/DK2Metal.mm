#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "metal_bridge/DK2BridgeProtocol.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr NSUInteger kFramesInFlight = 3;
std::atomic<uint64_t> gRequestedDrawableSize{0};
std::atomic<uint64_t> gRenderedFrames{0};
std::atomic<uint64_t> gBridgeFramesRendered{0};
uint64_t gSelfTestFrames = 0;
NSString *gBridgePath = nil;
bool gBridgeRequired = false;

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
        alert.messageText = @"Dungeon Keeper II requires Metal 4";
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
    }

    ~BridgeReader() {
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
            FrameSnapshot next;
            next.frame = frame;
            next.commandCount = slot->command_count;
            next.width = slot->width;
            next.height = slot->height;
            next.bytes.resize(byteCount);
            std::memcpy(next.bytes.data(), static_cast<uint8_t *>(mapping_) + DK2M_SLOT_OFFSET(slotIndex), byteCount);
            const uint32_t sequenceAfter = __atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE);
            if (sequenceBefore == sequenceAfter && (sequenceAfter & 1u) == 0) {
                snapshot_ = std::move(next);
                __atomic_store_n(&header_->consumer_frame, frame, __ATOMIC_RELEASE);
                NSLog(@"Bridge accepted frame %u (%u commands, %u bytes)", frame, snapshot_.commandCount, byteCount);
            }
        }
        return snapshot_.frame == 0 ? nullptr : &snapshot_;
    }

private:
    int descriptor_ = -1;
    void *mapping_ = nullptr;
    DK2MFileHeader *header_ = nullptr;
    FrameSnapshot snapshot_;
};

struct MetalVertex {
    float position[4];
    float color[4];
};

constexpr NSUInteger kVertexBufferSize = 2 * 1024 * 1024;
constexpr NSUInteger kIndexBufferSize = 512 * 1024;

} // namespace

@interface DK2MetalView : NSView
@end

@implementation DK2MetalView

- (CALayer *)makeBackingLayer {
    return [CAMetalLayer layer];
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
    }
    return self;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)layout {
    [super layout];
    NSRect backing = [self convertRectToBacking:self.bounds];
    gRequestedDrawableSize.store(packSize(backing.size), std::memory_order_release);
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
    id<MTLRenderPipelineState> _pipeline;
    id<MTLResidencySet> _resources;
    id<MTL4CommandAllocator> _allocators[kFramesInFlight];
    id<MTL4CommandBuffer> _commandBuffers[kFramesInFlight];
    id<MTLBuffer> _vertexBuffers[kFramesInFlight];
    id<MTLBuffer> _indexBuffers[kFramesInFlight];
    id<MTL4ArgumentTable> _argumentTables[kFramesInFlight];
    std::unique_ptr<BridgeReader> _bridge;
    uint64_t _frame;
    uint64_t _appliedDrawableSize;
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
    _layer.maximumDrawableCount = kFramesInFlight;
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
    id<MTLFunction> vertexFunction = [library newFunctionWithName:@"dk2_vertex"];
    id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"dk2_fragment"];
    MTLRenderPipelineDescriptor *pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineDescriptor.label = @"DK2 fixed-function base pipeline";
    pipelineDescriptor.vertexFunction = vertexFunction;
    pipelineDescriptor.fragmentFunction = fragmentFunction;
    pipelineDescriptor.colorAttachments[0].pixelFormat = _layer.pixelFormat;
    pipelineDescriptor.colorAttachments[0].blendingEnabled = YES;
    pipelineDescriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pipelineDescriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipelineDescriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pipelineDescriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    _pipeline = vertexFunction && fragmentFunction
                    ? [_device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error]
                    : nil;
    if (!_pipeline) {
        fail([NSString stringWithFormat:@"Metal shader pipeline failed: %@", error.localizedDescription ?: @"library missing"]);
        return nil;
    }

    MTLResidencySetDescriptor *residencyDescriptor = [[MTLResidencySetDescriptor alloc] init];
    residencyDescriptor.initialCapacity = kFramesInFlight * 2;
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
        _vertexBuffers[index].label = [NSString stringWithFormat:@"DK2 vertices %lu", index];
        _indexBuffers[index].label = [NSString stringWithFormat:@"DK2 indices %lu", index];

        MTL4ArgumentTableDescriptor *tableDescriptor = [[MTL4ArgumentTableDescriptor alloc] init];
        tableDescriptor.maxBufferBindCount = 1;
        tableDescriptor.initializeBindings = YES;
        tableDescriptor.label = [NSString stringWithFormat:@"DK2 arguments %lu", index];
        _argumentTables[index] = [_device newArgumentTableWithDescriptor:tableDescriptor error:&error];
        if (!_vertexBuffers[index] || !_indexBuffers[index] || !_argumentTables[index]) {
            fail(@"Metal dynamic frame buffer creation failed.");
            return nil;
        }
    }

    id<MTLAllocation> allocations[kFramesInFlight * 2];
    for (NSUInteger index = 0; index < kFramesInFlight; ++index) {
        allocations[index * 2] = _vertexBuffers[index];
        allocations[index * 2 + 1] = _indexBuffers[index];
    }
    [_resources addAllocations:allocations count:kFramesInFlight * 2];
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
        const NSUInteger slot = _frame % kFramesInFlight;
        if (_frame >= kFramesInFlight) {
            const uint64_t required = _frame - kFramesInFlight + 1;
            if (![_completed waitUntilSignaledValue:required timeoutMS:1000]) {
                fail(@"GPU frame completion timed out.");
                link.paused = YES;
                return;
            }
        }

        [_allocators[slot] reset];
        id<MTL4CommandBuffer> commandBuffer = _commandBuffers[slot];
        [commandBuffer beginCommandBufferWithAllocator:_allocators[slot]];

        const FrameSnapshot *snapshot = _bridge ? _bridge->poll() : nullptr;
        const double t = update.targetPresentationTimestamp;
        const double pulse = 0.5 + 0.5 * std::sin(t * 1.8);
        MTLClearColor clearColor = MTLClearColorMake(0.025 + pulse * 0.035, 0.07, 0.10 + pulse * 0.08, 1.0);
        if (snapshot) {
            size_t offset = 0;
            while (offset + sizeof(DK2MCommandHeader) <= snapshot->bytes.size()) {
                DK2MCommandHeader header;
                std::memcpy(&header, snapshot->bytes.data() + offset, sizeof(header));
                if (header.size < sizeof(header) || offset + header.size > snapshot->bytes.size()) break;
                if (header.type == DK2M_COMMAND_CLEAR && header.size == sizeof(DK2MClearCommand)) {
                    DK2MClearCommand clear;
                    std::memcpy(&clear, snapshot->bytes.data() + offset, sizeof(clear));
                    clearColor = MTLClearColorMake(clear.red, clear.green, clear.blue, clear.alpha);
                }
                offset += header.size;
            }
        }
        MTL4RenderPassDescriptor *pass = [[MTL4RenderPassDescriptor alloc] init];
        MTLRenderPassColorAttachmentDescriptor *color = pass.colorAttachments[0];
        color.texture = update.drawable.texture;
        color.loadAction = MTLLoadActionClear;
        color.storeAction = MTLStoreActionStore;
        color.clearColor = clearColor;

        id<MTL4RenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        encoder.label = @"DK2 native Metal frame";
        if (snapshot) {
            NSUInteger vertexOffset = 0;
            NSUInteger indexOffset = 0;
            size_t commandOffset = 0;
            while (commandOffset + sizeof(DK2MCommandHeader) <= snapshot->bytes.size()) {
                DK2MCommandHeader header;
                std::memcpy(&header, snapshot->bytes.data() + commandOffset, sizeof(header));
                if (header.size < sizeof(header) || commandOffset + header.size > snapshot->bytes.size()) break;
                if (header.type == DK2M_COMMAND_DRAW_INDEXED && header.size >= sizeof(DK2MDrawIndexedCommand)) {
                    DK2MDrawIndexedCommand draw;
                    std::memcpy(&draw, snapshot->bytes.data() + commandOffset, sizeof(draw));
                    const size_t vertexBytes = static_cast<size_t>(draw.vertex_count) * sizeof(DK2MVertex1C);
                    const size_t indexBytes = static_cast<size_t>(draw.index_count) * sizeof(uint16_t);
                    const size_t expected = sizeof(draw) + vertexBytes + indexBytes;
                    const NSUInteger metalVertexBytes = static_cast<NSUInteger>(draw.vertex_count) * sizeof(MetalVertex);
                    vertexOffset = (vertexOffset + 15u) & ~15u;
                    indexOffset = (indexOffset + 3u) & ~3u;
                    if (draw.fvf == DK2M_FVF_VERTEX1C && expected <= header.size &&
                        vertexOffset + metalVertexBytes <= kVertexBufferSize &&
                        indexOffset + indexBytes <= kIndexBufferSize && snapshot->width && snapshot->height) {
                        const uint8_t *rawVertices = snapshot->bytes.data() + commandOffset + sizeof(draw);
                        const uint8_t *rawIndices = rawVertices + vertexBytes;
                        auto *metalVertices = reinterpret_cast<MetalVertex *>(
                            static_cast<uint8_t *>(_vertexBuffers[slot].contents) + vertexOffset);
                        for (uint32_t index = 0; index < draw.vertex_count; ++index) {
                            DK2MVertex1C vertex;
                            std::memcpy(&vertex, rawVertices + index * sizeof(vertex), sizeof(vertex));
                            metalVertices[index].position[0] = vertex.x * 2.0f / snapshot->width - 1.0f;
                            metalVertices[index].position[1] = 1.0f - vertex.y * 2.0f / snapshot->height;
                            metalVertices[index].position[2] = vertex.z;
                            metalVertices[index].position[3] = 1.0f;
                            metalVertices[index].color[0] = ((vertex.diffuse >> 16) & 0xFF) / 255.0f;
                            metalVertices[index].color[1] = ((vertex.diffuse >> 8) & 0xFF) / 255.0f;
                            metalVertices[index].color[2] = (vertex.diffuse & 0xFF) / 255.0f;
                            metalVertices[index].color[3] = ((vertex.diffuse >> 24) & 0xFF) / 255.0f;
                        }
                        std::memcpy(static_cast<uint8_t *>(_indexBuffers[slot].contents) + indexOffset,
                                    rawIndices, indexBytes);
                        [_argumentTables[slot] setAddress:_vertexBuffers[slot].gpuAddress + vertexOffset atIndex:0];
                        [encoder setRenderPipelineState:_pipeline];
                        [encoder setArgumentTable:_argumentTables[slot] atStages:MTLRenderStageVertex];
                        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                            indexCount:draw.index_count
                                             indexType:MTLIndexTypeUInt16
                                           indexBuffer:_indexBuffers[slot].gpuAddress + indexOffset
                                     indexBufferLength:indexBytes];
                        vertexOffset += metalVertexBytes;
                        indexOffset += indexBytes;
                    }
                }
                commandOffset += header.size;
            }
            gBridgeFramesRendered.fetch_add(1, std::memory_order_relaxed);
        }
        [encoder endEncoding];
        [commandBuffer endCommandBuffer];

        [_queue waitForDrawable:update.drawable];
        id<MTL4CommandBuffer> submissions[] = {commandBuffer};
        [_queue commit:submissions count:1];
        [_queue signalEvent:_completed value:_frame + 1];
        [_queue signalDrawable:update.drawable];
        [update.drawable present];

        ++_frame;
        gRenderedFrames.store(_frame, std::memory_order_release);

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

    [_view layoutSubtreeIfNeeded];
    CAMetalLayer *layer = (CAMetalLayer *)_view.layer;
    NSRect backing = [_view convertRectToBacking:_view.bounds];
    layer.drawableSize = backing.size;
    gRequestedDrawableSize.store(packSize(backing.size), std::memory_order_release);

    _renderer = [[DK2MetalRenderer alloc] initWithLayer:layer];
    [_renderer start];
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
