#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "metal_bridge/DK2BridgeProtocol.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

constexpr NSUInteger kFramesInFlight = 3;
constexpr CGFloat kDrawableWidth = 2560.0;
constexpr CGFloat kDrawableHeight = 1920.0;
std::atomic<uint64_t> gRequestedDrawableSize{0};
std::atomic<uint64_t> gRenderedFrames{0};
std::atomic<uint64_t> gBridgeFramesRendered{0};
uint64_t gSelfTestFrames = 0;
NSString *gBridgePath = nil;
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
};

struct MetalVertex {
    float position[4];
    float color[4];
    float texCoord[2];
    uint32_t textureIndex;
    uint32_t padding;
};

constexpr NSUInteger kVertexBufferSize = 2 * 1024 * 1024;
constexpr NSUInteger kIndexBufferSize = 512 * 1024;
constexpr NSUInteger kTextureBindingsPerFrame = 128;
constexpr uint32_t kD3DRenderStateZEnable = 7;
constexpr uint32_t kD3DRenderStateZWriteEnable = 14;
constexpr uint32_t kD3DRenderStateSourceBlend = 19;
constexpr uint32_t kD3DRenderStateDestinationBlend = 20;
constexpr uint32_t kD3DRenderStateCullMode = 22;
constexpr uint32_t kD3DRenderStateZFunc = 23;
constexpr uint32_t kD3DRenderStateAlphaBlendEnable = 27;
static_assert(sizeof(MetalVertex) == 48);

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
    DK2MInputEvent *event = &_input.events[(write - 1) % 4];
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
    if ([self updatePointerFromEvent:event])
        [self setButton:0 pressed:YES doubleClick:event.clickCount >= 2];
}
- (void)mouseUp:(NSEvent *)event {
    [self updatePointerFromEvent:event];
    [self setButton:0 pressed:NO doubleClick:NO];
}
- (void)rightMouseDown:(NSEvent *)event {
    if ([self updatePointerFromEvent:event])
        [self setButton:1 pressed:YES doubleClick:event.clickCount >= 2];
}
- (void)rightMouseUp:(NSEvent *)event {
    [self updatePointerFromEvent:event];
    [self setButton:1 pressed:NO doubleClick:NO];
}
- (void)otherMouseDown:(NSEvent *)event {
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
    if (!_keyboard) [self setKey:dikForMacKeyCode(event.keyCode) pressed:YES];
}

- (void)keyUp:(NSEvent *)event {
    if (!_keyboard) [self setKey:dikForMacKeyCode(event.keyCode) pressed:NO];
}

- (void)flagsChanged:(NSEvent *)event {
    if (_keyboard) return;
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
    const CGFloat targetAspect = (CGFloat)kDrawableWidth / (CGFloat)kDrawableHeight;
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
    [CATransaction commit];
    [self.window invalidateCursorRectsForView:self];
    gRequestedDrawableSize.store(
        packSize(CGSizeMake(kDrawableWidth, kDrawableHeight)),
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
    id<MTLRenderPipelineState> _opaquePipeline;
    id<MTLRenderPipelineState> _alphaPipeline;
    id<MTLRenderPipelineState> _additivePipeline;
    id<MTLDepthStencilState> _depthStates[9][2];
    id<MTLSamplerState> _sampler;
    id<MTLTexture> _whiteTexture;
    id<MTLTexture> _depthTexture;
    NSMutableDictionary<NSNumber *, id<MTLTexture>> *_textures;
    id<MTLResidencySet> _resources;
    id<MTL4CommandAllocator> _allocators[kFramesInFlight];
    id<MTL4CommandBuffer> _commandBuffers[kFramesInFlight];
    id<MTLBuffer> _vertexBuffers[kFramesInFlight];
    id<MTLBuffer> _indexBuffers[kFramesInFlight];
    id<MTL4ArgumentTable> _argumentTables[kFramesInFlight];
    std::unique_ptr<BridgeReader> _bridge;
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
    id<MTLFunction> vertexFunction = [library newFunctionWithName:@"dk2_vertex"];
    id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"dk2_fragment"];
    MTLRenderPipelineDescriptor *pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineDescriptor.label = @"DK2 fixed-function base pipeline";
    pipelineDescriptor.vertexFunction = vertexFunction;
    pipelineDescriptor.fragmentFunction = fragmentFunction;
    pipelineDescriptor.colorAttachments[0].pixelFormat = _layer.pixelFormat;
    pipelineDescriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    pipelineDescriptor.colorAttachments[0].blendingEnabled = NO;
    _opaquePipeline = vertexFunction && fragmentFunction
                          ? [_device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error]
                          : nil;
    pipelineDescriptor.colorAttachments[0].blendingEnabled = YES;
    pipelineDescriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pipelineDescriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipelineDescriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pipelineDescriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    _alphaPipeline = vertexFunction && fragmentFunction
                         ? [_device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error]
                         : nil;
    pipelineDescriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    pipelineDescriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
    pipelineDescriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pipelineDescriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
    _additivePipeline = vertexFunction && fragmentFunction
                            ? [_device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error]
                            : nil;
    if (!_opaquePipeline || !_alphaPipeline || !_additivePipeline) {
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
    _textures = [NSMutableDictionary dictionary];
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
        _vertexBuffers[index].label = [NSString stringWithFormat:@"DK2 vertices %lu", index];
        _indexBuffers[index].label = [NSString stringWithFormat:@"DK2 indices %lu", index];

        MTL4ArgumentTableDescriptor *tableDescriptor = [[MTL4ArgumentTableDescriptor alloc] init];
        tableDescriptor.maxBufferBindCount = 1;
        tableDescriptor.maxTextureBindCount = kTextureBindingsPerFrame;
        tableDescriptor.maxSamplerStateBindCount = 1;
        tableDescriptor.initializeBindings = YES;
        tableDescriptor.label = [NSString stringWithFormat:@"DK2 arguments %lu", index];
        _argumentTables[index] = [_device newArgumentTableWithDescriptor:tableDescriptor error:&error];
        if (!_vertexBuffers[index] || !_indexBuffers[index] || !_argumentTables[index]) {
            fail(@"Metal dynamic frame buffer creation failed.");
            return nil;
        }
        [_argumentTables[index] setTexture:_whiteTexture.gpuResourceID atIndex:0];
        [_argumentTables[index] setSamplerState:_sampler.gpuResourceID atIndex:0];
    }

    id<MTLAllocation> allocations[kFramesInFlight * 2 + 1];
    for (NSUInteger index = 0; index < kFramesInFlight; ++index) {
        allocations[index * 2] = _vertexBuffers[index];
        allocations[index * 2 + 1] = _indexBuffers[index];
    }
    allocations[kFramesInFlight * 2] = _whiteTexture;
    [_resources addAllocations:allocations count:kFramesInFlight * 2 + 1];
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

- (BOOL)ensureDepthTextureWidth:(NSUInteger)width height:(NSUInteger)height {
    if (_depthTexture && _depthTexture.width == width && _depthTexture.height == height) return YES;
    if (_depthTexture) {
        if (_frame && ![_completed waitUntilSignaledValue:_frame timeoutMS:1000]) return NO;
        [_resources removeAllocation:_depthTexture];
    }
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                    width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget;
    _depthTexture = [_device newTextureWithDescriptor:descriptor];
    if (!_depthTexture) return NO;
    _depthTexture.label = @"DK2 depth buffer";
    [_resources addAllocation:_depthTexture];
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
        const FrameSnapshot *snapshot = _bridge ? _bridge->poll() : nullptr;
        if (gSelfTestFrames == 0 && _bridge && snapshot && snapshot->frame == _lastBridgeFrame) return;

        const NSUInteger slot = _frame % kFramesInFlight;
        if (_frame >= kFramesInFlight) {
            const uint64_t required = _frame - kFramesInFlight + 1;
            if (![_completed waitUntilSignaledValue:required timeoutMS:1000]) {
                fail(@"GPU frame completion timed out.");
                link.paused = YES;
                return;
            }
        }
        if (![self ensureDepthTextureWidth:update.drawable.texture.width
                                     height:update.drawable.texture.height]) {
            fail(@"Metal depth buffer creation failed.");
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
            size_t offset = 0;
            while (offset + sizeof(DK2MCommandHeader) <= snapshot->bytes.size()) {
                DK2MCommandHeader header;
                std::memcpy(&header, snapshot->bytes.data() + offset, sizeof(header));
                if (header.size < sizeof(header) || offset + header.size > snapshot->bytes.size()) break;
                if (header.type == DK2M_COMMAND_CLEAR && header.size == sizeof(DK2MClearCommand)) {
                    DK2MClearCommand clear;
                    std::memcpy(&clear, snapshot->bytes.data() + offset, sizeof(clear));
                    clearColor = MTLClearColorMake(clear.red, clear.green, clear.blue, clear.alpha);
                } else if (header.type == DK2M_COMMAND_TEXTURE_UPDATE &&
                           header.size >= sizeof(DK2MTextureUpdateCommand)) {
                    DK2MTextureUpdateCommand textureUpdate;
                    std::memcpy(&textureUpdate, snapshot->bytes.data() + offset, sizeof(textureUpdate));
                    const size_t expected = sizeof(textureUpdate) + textureUpdate.data_size;
                    NSNumber *key = @(textureUpdate.texture_id);
                    if (textureUpdate.texture_id && expected <= header.size &&
                        textureUpdate.width && textureUpdate.height &&
                        textureUpdate.row_pitch >= textureUpdate.width * 4 &&
                        textureUpdate.data_size >= textureUpdate.row_pitch * textureUpdate.height) {
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
                            const uint8_t *pixels = snapshot->bytes.data() + offset + sizeof(textureUpdate);
                            [texture replaceRegion:MTLRegionMake2D(0, 0, textureUpdate.width, textureUpdate.height)
                                       mipmapLevel:0 withBytes:pixels bytesPerRow:textureUpdate.row_pitch];
                        }
                    }
                }
                offset += header.size;
            }
        }
        if (residencyChanged) [_resources commit];
        MTL4RenderPassDescriptor *pass = [[MTL4RenderPassDescriptor alloc] init];
        MTLRenderPassColorAttachmentDescriptor *color = pass.colorAttachments[0];
        color.texture = update.drawable.texture;
        color.loadAction = MTLLoadActionClear;
        color.storeAction = MTLStoreActionStore;
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
            uint32_t currentTextureBinding = 0;
            uint32_t nextTextureBinding = 1;
            uint32_t zFunction = 4;
            BOOL zEnabled = YES;
            BOOL zWriteEnabled = YES;
            BOOL alphaBlendEnabled = NO;
            uint32_t sourceBlend = 2;
            uint32_t destinationBlend = 1;
            uint32_t cullMode = 1;
            std::unordered_map<uint32_t, uint32_t> textureBindings;
            [_argumentTables[slot] setAddress:_vertexBuffers[slot].gpuAddress atIndex:0];
            [_argumentTables[slot] setTexture:_whiteTexture.gpuResourceID atIndex:0];
            [_argumentTables[slot] setSamplerState:_sampler.gpuResourceID atIndex:0];
            [encoder setArgumentTable:_argumentTables[slot]
                             atStages:MTLRenderStageVertex | MTLRenderStageFragment];
            size_t commandOffset = 0;
            while (commandOffset + sizeof(DK2MCommandHeader) <= snapshot->bytes.size()) {
                DK2MCommandHeader header;
                std::memcpy(&header, snapshot->bytes.data() + commandOffset, sizeof(header));
                if (header.size < sizeof(header) || commandOffset + header.size > snapshot->bytes.size()) break;
                if (header.type == DK2M_COMMAND_SET_TEXTURE && header.size == sizeof(DK2MSetTextureCommand)) {
                    DK2MSetTextureCommand binding;
                    std::memcpy(&binding, snapshot->bytes.data() + commandOffset, sizeof(binding));
                    if (binding.stage == 0 && binding.texture_id) {
                        auto found = textureBindings.find(binding.texture_id);
                        if (found != textureBindings.end()) {
                            currentTextureBinding = found->second;
                        } else if (nextTextureBinding < kTextureBindingsPerFrame) {
                            id<MTLTexture> texture = _textures[@(binding.texture_id)];
                            if (texture) {
                                currentTextureBinding = nextTextureBinding++;
                                textureBindings.emplace(binding.texture_id, currentTextureBinding);
                                [_argumentTables[slot] setTexture:texture.gpuResourceID
                                                         atIndex:currentTextureBinding];
                            } else {
                                currentTextureBinding = 0;
                            }
                        }
                    } else if (binding.stage == 0) {
                        currentTextureBinding = 0;
                    }
                } else if (header.type == DK2M_COMMAND_RENDER_STATE &&
                           header.size == sizeof(DK2MRenderStateCommand)) {
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
                        default: break;
                    }
                } else if (header.type == DK2M_COMMAND_DRAW_INDEXED && header.size >= sizeof(DK2MDrawIndexedCommand)) {
                    DK2MDrawIndexedCommand draw;
                    std::memcpy(&draw, snapshot->bytes.data() + commandOffset, sizeof(draw));
                    const size_t vertexBytes = static_cast<size_t>(draw.vertex_count) * sizeof(DK2MVertex1C);
                    const size_t indexBytes = static_cast<size_t>(draw.index_count) * sizeof(uint16_t);
                    const size_t expected = sizeof(draw) + vertexBytes + indexBytes;
                    const NSUInteger metalVertexBytes = static_cast<NSUInteger>(draw.vertex_count) * sizeof(MetalVertex);
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
                            const float reciprocalW = std::abs(vertex.rhw) > 0.000001f ? vertex.rhw : 1.0f;
                            const float clipW = 1.0f / reciprocalW;
                            metalVertices[index].position[0] =
                                (vertex.x * 2.0f / snapshot->width - 1.0f) * clipW;
                            metalVertices[index].position[1] =
                                (1.0f - vertex.y * 2.0f / snapshot->height) * clipW;
                            metalVertices[index].position[2] = vertex.z * clipW;
                            metalVertices[index].position[3] = clipW;
                            metalVertices[index].color[0] = ((vertex.diffuse >> 16) & 0xFF) / 255.0f;
                            metalVertices[index].color[1] = ((vertex.diffuse >> 8) & 0xFF) / 255.0f;
                            metalVertices[index].color[2] = (vertex.diffuse & 0xFF) / 255.0f;
                            metalVertices[index].color[3] = ((vertex.diffuse >> 24) & 0xFF) / 255.0f;
                            metalVertices[index].texCoord[0] = vertex.u;
                            metalVertices[index].texCoord[1] = vertex.v;
                            metalVertices[index].textureIndex = currentTextureBinding;
                            metalVertices[index].padding = 0;
                        }
                        auto *metalIndices = reinterpret_cast<uint16_t *>(
                            static_cast<uint8_t *>(_indexBuffers[slot].contents) + indexOffset);
                        const auto *sourceIndices = reinterpret_cast<const uint16_t *>(rawIndices);
                        const uint32_t baseVertex = static_cast<uint32_t>(vertexOffset / sizeof(MetalVertex));
                        bool validIndices = true;
                        for (uint32_t index = 0; index < draw.index_count; ++index) {
                            const uint32_t adjusted = baseVertex + sourceIndices[index];
                            if (adjusted > UINT16_MAX) { validIndices = false; break; }
                            metalIndices[index] = static_cast<uint16_t>(adjusted);
                        }
                        if (!validIndices) { commandOffset += header.size; continue; }
                        id<MTLRenderPipelineState> pipeline = _opaquePipeline;
                        if (alphaBlendEnabled) {
                            pipeline = sourceBlend == 2 && destinationBlend == 2
                                           ? _additivePipeline : _alphaPipeline;
                        }
                        [encoder setRenderPipelineState:pipeline];
                        const uint32_t effectiveZFunction = zEnabled ? zFunction : 8;
                        const uint32_t effectiveZWrite = zEnabled && zWriteEnabled ? 1 : 0;
                        [encoder setDepthStencilState:_depthStates[effectiveZFunction][effectiveZWrite]];
                        [encoder setCullMode:cullMode == 1 ? MTLCullModeNone : MTLCullModeBack];
                        [encoder setFrontFacingWinding:cullMode == 3 ? MTLWindingClockwise
                                                                    : MTLWindingCounterClockwise];
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
        if (snapshot) _lastBridgeFrame = snapshot->frame;
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
    NSTask *_gameRunner;
    BOOL _terminating;
}

- (void)startBundledGameRunner {
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
    const CGSize drawableSize = CGSizeMake(kDrawableWidth, kDrawableHeight);
    layer.drawableSize = drawableSize;
    gRequestedDrawableSize.store(packSize(drawableSize), std::memory_order_release);

    _renderer = [[DK2MetalRenderer alloc] initWithLayer:layer];
    [_view publishCurrentInput];
    [_renderer start];
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
