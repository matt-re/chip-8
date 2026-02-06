#import <Cocoa/Cocoa.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#define PROGRAM_MAX_SIZE (0xEA0 - 0x200)
#define STACK_MAX_SIZE   32

/* Quirk flags */
enum chip8_quirks {
    CHIP8_QUIRK_NONE        = 0x00,
    CHIP8_QUIRK_SHIFT_VX    = 0x01, /* 8XY6 and 8XYE use VX for the source of the shift instead of VY */
    CHIP8_QUIRK_JUMP_FROM_X = 0x02, /* BNNN uses VX for jump offset intead of V0 */
    CHIP8_QUIRK_NO_CLIPPING = 0x04, /* DXYN wraps sprite instead of clipping at edges */
    CHIP8_QUIRK_INCREMENT_I = 0x08, /* FX55 and FX65 increments the I address */
    CHIP8_QUIRK_RESET_VF    = 0x10, /* 8XY1, 8XY2 and 8XY3 set VF to zero */
    CHIP8_QUIRK_VBLANK_WAIT = 0x20, /* DXYN a single sprite is drawn per VBLANK */
};

struct chip8_program {
    uint16_t pc;
    uint16_t sp;
    uint16_t stack;
    uint16_t i;
    uint16_t v;
    uint16_t bm;
    uint16_t len;
    uint8_t sound;
    uint8_t timer;
    uint8_t mem[0x1000];
};

struct chip8_opcode {
    uint16_t nnn;
    uint8_t nn;
    uint8_t n;
    uint8_t vx;
    uint8_t vy;
    uint8_t group;
};

/* Font data */
static uint8_t Fonts[] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0, /* Font 0 */
    0x20, 0x60, 0x20, 0x20, 0x70, /* Font 1 */
    0xF0, 0x10, 0xF0, 0x80, 0xF0, /* Font 2 */
    0xF0, 0x10, 0xF0, 0x10, 0xF0, /* Font 3 */
    0x90, 0x90, 0xF0, 0x10, 0x10, /* Font 4 */
    0xF0, 0x80, 0xF0, 0x10, 0xF0, /* Font 5 */
    0xF0, 0x80, 0xF0, 0x90, 0xF0, /* Font 6 */
    0xF0, 0x10, 0x20, 0x40, 0x40, /* Font 7 */
    0xF0, 0x90, 0xF0, 0x90, 0xF0, /* Font 8 */
    0xF0, 0x90, 0xF0, 0x10, 0xF0, /* Font 9 */
    0xF0, 0x90, 0xF0, 0x90, 0x90, /* Font A */
    0xE0, 0x90, 0xE0, 0x90, 0xE0, /* Font B */
    0xF0, 0x80, 0x80, 0x80, 0xF0, /* Font C */
    0xE0, 0x90, 0x90, 0x90, 0xE0, /* Font D */
    0xF0, 0x80, 0xF0, 0x80, 0xF0, /* Font E */
    0xF0, 0x80, 0xF0, 0x80, 0x80  /* Font F */
};

/* Demo program */
static uint8_t DemoRandomTimer[] = {
    0x00, 0xE0, 0xC0, 0x0F, 0xF0, 0x29, 0x61, 0x1C,
    0x62, 0x0E, 0xD1, 0x25, 0x63, 0x1E, 0xF3, 0x15,
    0xF4, 0x07, 0x34, 0x00, 0x12, 0x10, 0xD1, 0x25,
    0x12, 0x02
};

static struct chip8_opcode
opcode_from_bytes(uint8_t hi, uint8_t lo)
{
    uint16_t opcode = (uint16_t)((hi << 8) | lo);
    return (struct chip8_opcode) {
        .group = (uint8_t)((opcode & 0xF000) >> 12),
        .vx    = (uint8_t)((opcode & 0x0F00) >> 8),
        .vy    = (uint8_t)((opcode & 0x00F0) >> 4),
        .nnn   = (uint16_t)(opcode & 0x0FFF),
        .nn    = (uint8_t)(opcode & 0x00FF),
        .n     = (uint8_t)(opcode & 0x000F)
    };
}

static bool
chip8_init(struct chip8_program *program, uint8_t *data, size_t size)
{
    if (size > PROGRAM_MAX_SIZE) {
        return false;
    }
    memset(program, 0, sizeof *program);
    memcpy(program->mem, Fonts, sizeof Fonts);
    memcpy(program->mem + 0x200, data, size);
    program->pc    = 0x200;
    program->stack = 0xEA0;
    program->v     = 0xEF0;
    program->bm    = 0xF00;
    program->len   = (uint16_t)size;
    return true;
}

static bool
chip8_exec_frame(struct chip8_program *program, int ops_per_frame, uint16_t keys_down,
                 bool *needs_beep, bool *key_held, uint8_t *held_key, enum chip8_quirks quirks)
{
    uint8_t  *mem    = program->mem;
    uint8_t  *v      = &mem[program->v];
    uint8_t  *stack  = &mem[program->stack];
    uint8_t  *bitmap = &mem[program->bm];
    uint16_t last_pc;
    uint16_t temp;

    *needs_beep = false;

    for (int i = 0; i < ops_per_frame; i++) {
        if (program->pc < 0x200 || program->pc >= 0xE9F) {
            return false;
        }
        last_pc = program->pc;

        struct chip8_opcode opcode = opcode_from_bytes(mem[program->pc], mem[program->pc + 1]);
        switch (opcode.group) {
        case 0x0:
            switch (opcode.nnn) {
            case 0xE0:
                memset(bitmap, 0, 256);
                program->pc += 2;
                break;
            case 0xEE:
                if (program->sp < 2) {
                    return false;
                }
                program->sp -= 2;
                program->pc = (uint16_t)((stack[program->sp] << 8 | stack[program->sp + 1]) & 0xFFFF);
                break;
            default:
                program->pc += 2;
                break;
            }
            break;
        case 0x1:
            program->pc = opcode.nnn;
            break;
        case 0x2:
            if (program->sp + 2 > STACK_MAX_SIZE) {
                return false;
            }
            stack[program->sp + 0] = (uint8_t)((program->pc + 2) >> 8);
            stack[program->sp + 1] = (uint8_t)((program->pc + 2) & 0xFF);
            program->sp += 2;
            program->pc = opcode.nnn;
            break;
        case 0x3:
            program->pc += v[opcode.vx] == opcode.nn ? 4 : 2;
            break;
        case 0x4:
            program->pc += v[opcode.vx] != opcode.nn ? 4 : 2;
            break;
        case 0x5:
            program->pc += v[opcode.vx] == v[opcode.vy] ? 4 : 2;
            break;
        case 0x6:
            v[opcode.vx] = opcode.nn;
            program->pc += 2;
            break;
        case 0x7:
            v[opcode.vx] += opcode.nn;
            program->pc += 2;
            break;
        case 0x8:
            switch (opcode.n) {
            case 0x0:
                v[opcode.vx] = v[opcode.vy];
                program->pc += 2;
                break;
            case 0x1:
                v[opcode.vx] |= v[opcode.vy];
                if (quirks & CHIP8_QUIRK_RESET_VF) {
                    v[0xF] = 0;
                }
                program->pc += 2;
                break;
            case 0x2:
                v[opcode.vx] &= v[opcode.vy];
                if (quirks & CHIP8_QUIRK_RESET_VF) {
                    v[0xF] = 0;
                }
                program->pc += 2;
                break;
            case 0x3:
                v[opcode.vx] ^= v[opcode.vy];
                if (quirks & CHIP8_QUIRK_RESET_VF) {
                    v[0xF] = 0;
                }
                program->pc += 2;
                break;
            case 0x4:
                temp = (uint16_t)(v[opcode.vx] + v[opcode.vy]);
                v[opcode.vx] = temp & 0xFF;
                v[0xF] = !!(temp & 0xFF00);
                program->pc += 2;
                break;
            case 0x5:
                temp = (uint16_t)(v[opcode.vx] - v[opcode.vy]);
                v[opcode.vx] = temp & 0xFF;
                v[0xF] = (uint8_t)!((temp & 0x8000) >> 15);
                program->pc += 2;
                break;
            case 0x6:
                temp = (quirks & CHIP8_QUIRK_SHIFT_VX) ? v[opcode.vx] : v[opcode.vy];
                v[opcode.vx] = (temp >> 1) & 0xFF;
                v[0xF] = temp & 1;
                program->pc += 2;
                break;
            case 0x7:
                temp = (uint16_t)(v[opcode.vy] - v[opcode.vx]);
                v[opcode.vx] = temp & 0xFF;
                v[0xF] = (uint8_t)!((temp & 0x8000) >> 15);
                program->pc += 2;
                break;
            case 0xE:
                temp = (quirks & CHIP8_QUIRK_SHIFT_VX) ? v[opcode.vx] : v[opcode.vy];
                v[opcode.vx] = (temp << 1) & 0xFF;
                v[0xF] = (temp & 0x80) >> 7;
                program->pc += 2;
                break;
            }
            break;
        case 0x9:
            program->pc += v[opcode.vx] != v[opcode.vy] ? 4 : 2;
            break;
        case 0xA:
            program->i = opcode.nnn;
            program->pc += 2;
            break;
        case 0xB:
            if (quirks & CHIP8_QUIRK_JUMP_FROM_X) {
                program->pc = opcode.nnn + v[opcode.vx];
            } else {
                program->pc = opcode.nnn + v[0];
            }
            break;
        case 0xC:
            v[opcode.vx] = (uint8_t)(arc4random_uniform(256) & opcode.nn);
            program->pc += 2;
            break;
        case 0xD: {
            uint8_t x0 = v[opcode.vx] % 64;
            uint8_t y0 = v[opcode.vy] % 32;
            v[0xF] = 0;
            for (uint8_t y = 0; y < opcode.n; y++) {
                uint8_t yc = y0 + y;
                if (yc >= 32) {
                    if (quirks & CHIP8_QUIRK_NO_CLIPPING) {
                        yc %= 32;
                    } else {
                        break;
                    }
                }
                uint8_t sprite = mem[(program->i + y) & 0xFFF];
                for (uint8_t sprite_mask = 1 << 7, x = 0; sprite_mask != 0; sprite_mask >>= 1, x++) {
                    if (!(sprite & sprite_mask)) {
                        continue;
                    }
                    uint8_t xc = x0 + x;
                    if (xc >= 64) {
                        if (quirks & CHIP8_QUIRK_NO_CLIPPING) {
                            xc %= 64;
                        } else {
                            break;
                        }
                    }
                    uint16_t byte = (uint16_t)((yc * 64 + xc) / 8);
                    uint8_t mask = (uint8_t)(1 << (7 - xc % 8));
                    v[0xF] |= !!(bitmap[byte] & mask);
                    bitmap[byte] ^= mask;
                }
            }
            program->pc += 2;
            if (quirks & CHIP8_QUIRK_VBLANK_WAIT) {
                goto frame_done;
            }
            break;
        }
        case 0xE:
            switch (opcode.nn) {
            case 0x9E:
                program->pc += (keys_down & (1 << (v[opcode.vx] & 0xF))) ? 4 : 2;
                break;
            case 0xA1:
                program->pc += (keys_down & (1 << (v[opcode.vx] & 0xF))) ? 2 : 4;
                break;
            }
            break;
        case 0xF:
            switch (opcode.nn) {
            case 0x07:
                v[opcode.vx] = program->timer;
                program->pc += 2;
                break;
            case 0x0A:
                if (*key_held) {
                    /* Wait for key release */
                    if (!(keys_down & (1 << (*held_key & 0xF)))) {
                        *key_held = false;
                        program->pc += 2;
                    }
                } else if (keys_down) {
                    v[opcode.vx] = (uint8_t)(__builtin_ctz(keys_down) & 0xF);
                    *held_key = v[opcode.vx];
                    *key_held = true;
                }
                break;
            case 0x15:
                program->timer = v[opcode.vx];
                program->pc += 2;
                break;
            case 0x18:
                program->sound = v[opcode.vx];
                program->pc += 2;
                break;
            case 0x1E:
                program->i = (program->i + v[opcode.vx]) & 0xFFF;
                program->pc += 2;
                break;
            case 0x29:
                program->i = (uint16_t)(((v[opcode.vx] & 0xF) * 5) & 0xFFF);
                program->pc += 2;
                break;
            case 0x33:
                mem[(program->i + 0) & 0xFFF] = v[opcode.vx] / 100;
                mem[(program->i + 1) & 0xFFF] = v[opcode.vx] / 10 % 10;
                mem[(program->i + 2) & 0xFFF] = v[opcode.vx] % 10;
                program->pc += 2;
                break;
            case 0x55:
                for (uint8_t x = 0; x <= opcode.vx; x++) {
                    mem[(program->i + x) & 0xFFF] = v[x];
                }
                if (quirks & CHIP8_QUIRK_INCREMENT_I) {
                    program->i = (program->i + opcode.vx + 1) & 0xFFF;
                }
                program->pc += 2;
                break;
            case 0x65:
                for (uint8_t x = 0; x <= opcode.vx; x++) {
                    v[x] = mem[(program->i + x) & 0xFFF];
                }
                if (quirks & CHIP8_QUIRK_INCREMENT_I) {
                    program->i = (program->i + opcode.vx + 1) & 0xFFF;
                }
                program->pc += 2;
                break;
            }
            break;
        }

        /* Check for infinite loop or halt */
        if (last_pc == program->pc) {
            bool wait = opcode.group == 0xF && opcode.nn == 0x0A;
            bool halt = opcode.group == 0x1 && opcode.nnn == program->pc;
            if (halt) {
                return false;
            }
            if (!wait) {
                return false;
            }
            break;
        }
    }

frame_done:
    if (program->timer) {
        --program->timer;
    }

    if (program->sound) {
        *needs_beep = true;
        --program->sound;
    }

    return true;
}

/* Cocoa View */
@interface Chip8View : NSView <NSDraggingDestination>
{
    struct chip8_program program;
    uint16_t keysDown;
    NSTimer *timer;
    BOOL halted;
    BOOL keyHeld;
    uint8_t heldKey;
    CGContextRef bitmapContext;
    uint32_t *pixelBuffer;
}
@end

@implementation Chip8View

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self) {
        keysDown = 0;
        halted = NO;
        keyHeld = NO;
        heldKey = 0;

        /* Allocate pixel buffer for 64x32 RGBA pixels */
        pixelBuffer = (uint32_t *)malloc(64 * 32 * sizeof(uint32_t));

        /* Create bitmap context */
        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        bitmapContext = CGBitmapContextCreate(pixelBuffer,
                                              64, 32,
                                              8, 64 * 4,
                                              colorSpace,
                                              (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
        CGColorSpaceRelease(colorSpace);

        /* Load demo program */
        chip8_init(&program, DemoRandomTimer, sizeof DemoRandomTimer);

        /* Register for drag and drop */
        [self registerForDraggedTypes:@[NSPasteboardTypeFileURL]];

        /* Start 60fps timer */
        timer = [NSTimer scheduledTimerWithTimeInterval:1.0/60.0
                                                 target:self
                                               selector:@selector(tick:)
                                               userInfo:nil
                                                repeats:YES];
    }
    return self;
}

- (void)dealloc
{
    if (bitmapContext) {
        CGContextRelease(bitmapContext);
    }
    if (pixelBuffer) {
        free(pixelBuffer);
    }
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (void)updatePixelBuffer
{
    uint8_t *video = &program.mem[0xF00];

    /* Colors in ABGR order (little-endian uint32_t for RGBA memory layout) */
    /* Bright green: R=0, G=230, B=0, A=255 */
    /* Dark green: R=0, G=51, B=0, A=255 */
    const uint32_t fgColor = 0xFF00E600;
    const uint32_t bgColor = 0xFF003300;

    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 64; x++) {
            int bitIndex = y * 64 + x;
            uint8_t byte = video[bitIndex / 8];
            uint8_t mask = (uint8_t)(1 << (7 - (x % 8)));
            BOOL pixelOn = (byte & mask) != 0;

            pixelBuffer[y * 64 + x] = pixelOn ? fgColor : bgColor;
        }
    }
}

- (void)tick:(NSTimer *)t
{
    (void)t;
    if (halted) {
        return;
    }

    bool needs_beep = false;
    bool key_held = keyHeld;
    uint8_t held_key = heldKey;

    bool ok = chip8_exec_frame(&program, 10, keysDown, &needs_beep,
                               &key_held, &held_key, CHIP8_QUIRK_SHIFT_VX);

    keyHeld = key_held;
    heldKey = held_key;

    if (needs_beep) {
        NSBeep();
    }

    if (!ok) {
        halted = YES;
    }

    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirtyRect
{
    (void)dirtyRect;

    /* Fill with black (letterbox) */
    [[NSColor blackColor] setFill];
    NSRectFill(self.bounds);

    /* Border padding around display */
    CGFloat borderSize = 8.0;

    /* Calculate available area after border */
    CGFloat viewWidth = self.bounds.size.width;
    CGFloat viewHeight = self.bounds.size.height;
    CGFloat availWidth = viewWidth - borderSize * 2;
    CGFloat availHeight = viewHeight - borderSize * 2;

    /* Calculate display rect maintaining 2:1 aspect ratio */
    CGFloat targetAspect = 2.0;
    CGFloat availAspect = availWidth / availHeight;

    CGFloat displayWidth, displayHeight;
    if (availAspect > targetAspect) {
        /* Available area is wider than 2:1, letterbox on sides */
        displayHeight = availHeight;
        displayWidth = displayHeight * targetAspect;
    } else {
        /* Available area is taller than 2:1, letterbox on top/bottom */
        displayWidth = availWidth;
        displayHeight = displayWidth / targetAspect;
    }

    /* Center display within available area, offset by border */
    CGFloat displayX = borderSize + (availWidth - displayWidth) / 2.0;
    CGFloat displayY = borderSize + (availHeight - displayHeight) / 2.0;

    /* Draw border around display area */
    NSRect borderRect = NSMakeRect(displayX - 1, displayY - 1,
                                   displayWidth + 2, displayHeight + 2);
    [[NSColor colorWithWhite:0.3 alpha:1.0] setStroke];
    NSFrameRect(borderRect);

    /* Update pixel buffer from video memory */
    [self updatePixelBuffer];

    /* Create image from bitmap context */
    CGImageRef image = CGBitmapContextCreateImage(bitmapContext);
    if (image) {
        CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];

        /* Disable interpolation for crisp pixels */
        CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);

        /* Draw scaled image */
        CGRect displayRect = CGRectMake(displayX, displayY, displayWidth, displayHeight);
        CGContextDrawImage(ctx, displayRect, image);

        CGImageRelease(image);
    }
}

/* Keyboard mapping */
- (int)chip8KeyFromKeyCode:(unsigned short)keyCode
{
    /*
     * CHIP-8:  1 2 3 C    QWERTY:  1 2 3 4
     *          4 5 6 D             Q W E R
     *          7 8 9 E             A S D F
     *          A 0 B F             Z X C V
     */
    switch (keyCode) {
        case 18: return 0x1;  /* 1 */
        case 19: return 0x2;  /* 2 */
        case 20: return 0x3;  /* 3 */
        case 21: return 0xC;  /* 4 */
        case 12: return 0x4;  /* Q */
        case 13: return 0x5;  /* W */
        case 14: return 0x6;  /* E */
        case 15: return 0xD;  /* R */
        case 0:  return 0x7;  /* A */
        case 1:  return 0x8;  /* S */
        case 2:  return 0x9;  /* D */
        case 3:  return 0xE;  /* F */
        case 6:  return 0xA;  /* Z */
        case 7:  return 0x0;  /* X */
        case 8:  return 0xB;  /* C */
        case 9:  return 0xF;  /* V */
        default: return -1;
    }
}

- (void)keyDown:(NSEvent *)event
{
    int key = [self chip8KeyFromKeyCode:event.keyCode];
    if (key >= 0) {
        keysDown |= (1 << key);
    }
}

- (void)keyUp:(NSEvent *)event
{
    int key = [self chip8KeyFromKeyCode:event.keyCode];
    if (key >= 0) {
        keysDown &= ~(1 << key);
    }
}

/* Drag and Drop */
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    NSPasteboard *pb = [sender draggingPasteboard];
    if ([pb.types containsObject:NSPasteboardTypeFileURL]) {
        return NSDragOperationCopy;
    }
    return NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    NSPasteboard *pb = [sender draggingPasteboard];
    NSURL *fileURL = [NSURL URLFromPasteboard:pb];
    if (!fileURL) {
        return NO;
    }

    NSData *data = [NSData dataWithContentsOfURL:fileURL];
    if (!data || data.length == 0 || data.length > PROGRAM_MAX_SIZE) {
        return NO;
    }

    /* Reset emulator with new ROM */
    if (chip8_init(&program, (uint8_t *)data.bytes, data.length)) {
        halted = NO;
        keysDown = 0;
        keyHeld = NO;
        heldKey = 0;
        [self setNeedsDisplay:YES];
        return YES;
    }

    return NO;
}

@end

/* App Delegate */
@interface AppDelegate : NSObject <NSApplicationDelegate>
@property (strong) NSWindow *window;
@property (strong) Chip8View *chip8View;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    (void)notification;

    /* Create window */
    NSRect frame = NSMakeRect(100, 100, 640, 320);
    NSWindowStyleMask style = NSWindowStyleMaskTitled |
                              NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable |
                              NSWindowStyleMaskResizable;
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    [self.window setTitle:@"CHIP-8"];
    [self.window setContentAspectRatio:NSMakeSize(2.0, 1.0)];

    /* Create view */
    self.chip8View = [[Chip8View alloc] initWithFrame:frame];
    [self.window setContentView:self.chip8View];
    [self.window makeFirstResponder:self.chip8View];

    [self.window makeKeyAndOrderFront:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
    (void)sender;
    return YES;
}

@end

/* Main */
int main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;

    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        AppDelegate *delegate = [[AppDelegate alloc] init];
        [app setDelegate:delegate];

        /* Create menu bar */
        NSMenu *menuBar = [[NSMenu alloc] init];
        NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
        [menuBar addItem:appMenuItem];
        [app setMainMenu:menuBar];

        NSMenu *appMenu = [[NSMenu alloc] init];
        NSMenuItem *quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit CHIP-8"
                                                          action:@selector(terminate:)
                                                   keyEquivalent:@"q"];
        [appMenu addItem:quitItem];
        [appMenuItem setSubmenu:appMenu];

        [app activateIgnoringOtherApps:YES];
        [app run];
    }

    return 0;
}
