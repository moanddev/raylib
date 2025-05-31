/**********************************************************************************************
*
*   rcore_ios.c - Native iOS platform core module
*
*   Copyright (c) 2013-2024 Ramon Santamaria (@raysan5) and contributors
*
*   This software is provided "as-is", without any express or implied warranty. In no event
*   will the authors be held liable for any damages arising from the use of this software.
*
*   Permission is granted to anyone to use this software for any purpose, including commercial
*   applications, and to alter it and redistribute it freely, subject to the following restrictions:
*
*     1. The origin of this software must not be misrepresented; you must not claim that you
*     wrote the original software. If you use this software in a product, an acknowledgment
*     in the product documentation would be appreciated but is not required.
*
*     2. Altered source versions must be plainly marked as such, and must not be misrepresented
*     as being the original software.
*
*     3. This notice may not be removed or altered from any source distribution.
*
**********************************************************************************************/

#include "../raylib.h"       // raylib main header
#include "../utils.h"       // TRACELOG macros
// #include "rlgl.h"        // raylib OpenGL abstraction layer, included by raylib.h

#if defined(PLATFORM_IOS)

#include <UIKit/UIKit.h>
#include <OpenGLES/ES2/gl.h>
#include <OpenGLES/ES2/glext.h>
#include <QuartzCore/CADisplayLink.h> // Required for CADisplayLink

//----------------------------------------------------------------------------------
// Module Variables Definition (local)
//----------------------------------------------------------------------------------
extern CoreData CORE;                   // Global CORE state context (rcore.c)

static UIWindow *iosWindow = NULL;
static UIViewController *iosViewController = NULL;
static EAGLContext *iosEaglContext = NULL;
static CAEAGLLayer *iosEaglLayer = NULL;

static GLuint screenFramebuffer = 0;
static GLuint colorRenderbuffer = 0;
static GLuint depthRenderbuffer = 0; // Optional: For depth testing

static int currentScreenWidth = 0;    // Store current screen width (updated on InitGraphicsDevice and SwapScreenBuffer)
static int currentScreenHeight = 0;   // Store current screen height (updated on InitGraphicsDevice and SwapScreenBuffer)

// Game loop integration
static CADisplayLink *displayLink = NULL;
static bool appActive = true; // Manage application active state

// Forward declaration for C function called by Objective-C
void RaylibiOS_GameLoopStep(void);


// Custom UIView for touch handling and CADisplayLink target
@interface RaylibView : UIView
- (void)iosRunLoop:(CADisplayLink *)sender;
@end

static RaylibView *iosView = NULL; // Changed from UIView* to RaylibView*

//----------------------------------------------------------------------------------
// Module Functions Declaration - Platform Core (Init/Close)
//----------------------------------------------------------------------------------
void InitPlatform(void);
void ClosePlatform(void);
void PausePlatform(void); // App lifecycle
void ResumePlatform(void); // App lifecycle


//----------------------------------------------------------------------------------
// Module Functions Declaration - Graphics Device (Init/Close)
//----------------------------------------------------------------------------------
void InitGraphicsDevice(void);     // Initialize graphics device
void CloseGraphicsDevice(void);    // Close graphics device
void MakeContextCurrent(void);     // Make the graphics context current
// void SwapScreenBuffers(void);   // Swap front and back buffers (double buffering) - name changed to SwapScreenBuffer

//----------------------------------------------------------------------------------
// Module Functions Declaration - Screen drawing functions
//----------------------------------------------------------------------------------
void BeginDrawing(void);
void EndDrawing(void);
void SwapScreenBuffer(void);      // Renamed from SwapScreenBuffers for consistency with other platforms


// Helper to find or allocate a touch slot
static int GetTouchPointSlot(UITouch *touch) {
    intptr_t touchId = (intptr_t)touch;
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
        if (CORE.Input.Touch.pointId[i] == touchId) return i; // Found existing
    }
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
        if (CORE.Input.Touch.pointId[i] == 0) { // Found empty slot
            CORE.Input.Touch.pointId[i] = touchId;
            return i;
        }
    }
    TRACELOG(LOG_WARNING, "IOS: Maximum touch points reached, new touch not tracked.");
    return -1; // No slot available
}

static void UpdateMouseWithFirstTouch(void) {
    bool firstTouchFound = false;
    int activeTouchCount = 0;

    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
        if (CORE.Input.Touch.pointId[i] != 0 && CORE.Input.Touch.currentTouchState[i] == 1) {
            if (!firstTouchFound) {
                CORE.Input.Mouse.currentPosition.x = CORE.Input.Touch.position[i].x;
                CORE.Input.Mouse.currentPosition.y = CORE.Input.Touch.position[i].y;
                CORE.Input.Mouse.currentButtonState[MOUSE_BUTTON_LEFT] = 1;
                firstTouchFound = true;
            }
            activeTouchCount++;
        }
    }

    CORE.Input.Touch.pointCount = activeTouchCount;

    if (!firstTouchFound) {
        CORE.Input.Mouse.currentButtonState[MOUSE_BUTTON_LEFT] = 0;
    }
}

@implementation RaylibView

+ (Class)layerClass {
    return [CAEAGLLayer class];
}

- (id)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        [self setMultipleTouchEnabled:YES];
        CAEAGLLayer *eaglLayer = (CAEAGLLayer *)self.layer;
        eaglLayer.opaque = YES;
        eaglLayer.drawableProperties = @{
            kEAGLDrawablePropertyRetainedBacking: [NSNumber numberWithBool:NO], // Set to NO for performance if contents are redrawn every frame
            kEAGLDrawablePropertyColorFormat: kEAGLColorFormatRGBA8
        };
    }
    return self;
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch *touch in touches) {
        int slot = GetTouchPointSlot(touch);
        if (slot != -1) {
            CGPoint location = [touch locationInView:self];
            CORE.Input.Touch.position[slot].x = location.x * [[UIScreen mainScreen] scale]; // Adjust for DPI
            CORE.Input.Touch.position[slot].y = location.y * [[UIScreen mainScreen] scale]; // Adjust for DPI
            CORE.Input.Touch.currentTouchState[slot] = 1;
            TRACELOG(LOG_DEBUG, "IOS: Touch began (slot %d, id %p) at: %.2f, %.2f", slot, touch, location.x, location.y);
        }
    }
    UpdateMouseWithFirstTouch();
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch *touch in touches) {
        int slot = GetTouchPointSlot(touch);
        if (slot != -1) {
            CGPoint location = [touch locationInView:self];
            CORE.Input.Touch.position[slot].x = location.x * [[UIScreen mainScreen] scale]; // Adjust for DPI
            CORE.Input.Touch.position[slot].y = location.y * [[UIScreen mainScreen] scale]; // Adjust for DPI
            TRACELOG(LOG_DEBUG, "IOS: Touch moved (slot %d, id %p) at: %.2f, %.2f", slot, touch, location.x, location.y);
        }
    }
    UpdateMouseWithFirstTouch();
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch *touch in touches) {
        int slot = GetTouchPointSlot(touch);
        if (slot != -1) {
            CORE.Input.Touch.currentTouchState[slot] = 0;
            CORE.Input.Touch.pointId[slot] = 0;
            TRACELOG(LOG_DEBUG, "IOS: Touch ended (slot %d, id %p)", slot, touch);
        }
    }
    UpdateMouseWithFirstTouch();
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    TRACELOG(LOG_DEBUG, "IOS: Touch cancelled");
    [self touchesEnded:touches withEvent:event];
}

- (void)iosRunLoop:(CADisplayLink *)sender {
    RaylibiOS_GameLoopStep();
}

@end // RaylibView implementation

void RaylibiOS_GameLoopStep(void) {
    if (!appActive || CORE.Window.shouldClose) return;

    PollInputEvents();

    CORE.Time.current = GetTime();
    CORE.Time.update = CORE.Time.current - CORE.Time.previous;
    CORE.Time.previous = CORE.Time.current;

    if (CORE.Loop.callback != NULL)
    {
        CORE.Loop.callback();
    }

    CORE.Time.draw = GetTime() - CORE.Time.previous;
    CORE.Time.frame = CORE.Time.update + CORE.Time.draw;
    CORE.Time.frameCounter++;
}

void InitPlatform(void)
{
    CGRect screenBounds = [[UIScreen mainScreen] bounds];
    iosWindow = [[UIWindow alloc] initWithFrame:screenBounds];
    if (!iosWindow) { TRACELOG(LOG_FATAL, "PLATFORM: IOS: Failed to create UIWindow"); return; }

    iosView = [[RaylibView alloc] initWithFrame:screenBounds];
    if (!iosView) { TRACELOG(LOG_FATAL, "PLATFORM: IOS: Failed to create RaylibView"); [iosWindow release]; iosWindow = NULL; return; }

    iosViewController = [[UIViewController alloc] init];
    if (!iosViewController) { TRACELOG(LOG_FATAL, "PLATFORM: IOS: Failed to create UIViewController"); [iosView release]; iosView = NULL; [iosWindow release]; iosWindow = NULL; return; }

    [iosViewController setView:iosView];
    [iosWindow setRootViewController:iosViewController];
    [iosWindow makeKeyAndVisible];

    CORE.Window.display.width = (int)screenBounds.size.width;
    CORE.Window.display.height = (int)screenBounds.size.height;

    if (CORE.Window.screen.width == 0 || CORE.Window.screen.height == 0)
    {
        CORE.Window.screen.width = CORE.Window.display.width;
        CORE.Window.screen.height = CORE.Window.display.height;
    }

    for (int i = 0; i < MAX_TOUCH_POINTS; i++) CORE.Input.Touch.pointId[i] = 0;
    CORE.Input.Touch.pointCount = 0;

    appActive = true;

    displayLink = [CADisplayLink displayLinkWithTarget:iosView selector:@selector(iosRunLoop:)];
    if (!displayLink) { TRACELOG(LOG_FATAL, "PLATFORM: IOS: Failed to create CADisplayLink"); return; }

    int fps = (CORE.Time.target > 0.0) ? (int)(1.0/CORE.Time.target) : 60;
    if ([displayLink respondsToSelector:@selector(setPreferredFramesPerSecond:)]) {
        [displayLink setPreferredFramesPerSecond:fps];
    }

    [displayLink addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
    TRACELOG(LOG_INFO, "PLATFORM: IOS: CADisplayLink initialized for game loop at %d FPS", fps);
    TRACELOG(LOG_INFO, "PLATFORM: IOS: Initialized main window, RaylibView, and view controller");
}

void ClosePlatform(void)
{
    if (displayLink) { [displayLink invalidate]; displayLink = NULL; }
    if (iosViewController != NULL) [iosViewController release];
    if (iosView != NULL) [iosView release];
    if (iosWindow != NULL) [iosWindow release];
    iosViewController = NULL; iosView = NULL; iosWindow = NULL;
    TRACELOG(LOG_INFO, "PLATFORM: IOS: Closed main window, view, view controller, and display link");
}

void PausePlatform(void)
{
    if (displayLink) displayLink.paused = YES;
    appActive = false;
    TRACELOG(LOG_INFO, "PLATFORM: IOS: Paused display link.");
}

void ResumePlatform(void)
{
    if (displayLink) displayLink.paused = NO;
    appActive = true;
    TRACELOG(LOG_INFO, "PLATFORM: IOS: Resumed display link.");
}

void InitGraphicsDevice(void)
{
    if (!iosView) { TRACELOG(LOG_FATAL, "PLATFORM: IOS: View not initialized before graphics device"); return; }

    iosEaglLayer = (CAEAGLLayer *)[iosView layer];
    iosEaglContext = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
    if (!iosEaglContext || ![EAGLContext setCurrentContext:iosEaglContext])
    {
        TRACELOG(LOG_FATAL, "PLATFORM: IOS: Failed to create or set EAGLContext");
        if (iosEaglContext) [iosEaglContext release]; iosEaglContext = NULL; return;
    }

    glGenFramebuffers(1, &screenFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, screenFramebuffer);
    glGenRenderbuffers(1, &colorRenderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, colorRenderbuffer);

    if (![iosEaglContext renderbufferStorage:GL_RENDERBUFFER fromDrawable:iosEaglLayer])
    {
        TRACELOG(LOG_FATAL, "PLATFORM: IOS: Failed to allocate renderbuffer storage. GL Error: 0x%X", glGetError());
        glDeleteRenderbuffers(1, &colorRenderbuffer); glDeleteFramebuffers(1, &screenFramebuffer);
        [EAGLContext setCurrentContext:nil]; [iosEaglContext release]; iosEaglContext = NULL; return;
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorRenderbuffer);

    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &currentScreenWidth);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &currentScreenHeight);

    CORE.Window.screen.width = currentScreenWidth; CORE.Window.screen.height = currentScreenHeight;
    CORE.Window.render.width = currentScreenWidth; CORE.Window.render.height = currentScreenHeight;
    CORE.Window.currentFbo.width = currentScreenWidth; CORE.Window.currentFbo.height = currentScreenHeight;

    if (CORE.Window.flags & FLAG_DEPTH_TEST)
    {
        glGenRenderbuffers(1, &depthRenderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, depthRenderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, currentScreenWidth, currentScreenHeight);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRenderbuffer);
    }

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        TRACELOG(LOG_FATAL, "PLATFORM: IOS: Framebuffer is not complete (status: 0x%x)", status);
        if (depthRenderbuffer) glDeleteRenderbuffers(1, &depthRenderbuffer);
        glDeleteRenderbuffers(1, &colorRenderbuffer); glDeleteFramebuffers(1, &screenFramebuffer);
        [EAGLContext setCurrentContext:nil]; [iosEaglContext release]; iosEaglContext = NULL; return;
    }

    CORE.Window.ready = true;
    TRACELOG(LOG_INFO, "PLATFORM: IOS: Initialized graphics device (Screen: %ix%i)", currentScreenWidth, currentScreenHeight);
    glViewport(0, 0, currentScreenWidth, currentScreenHeight);
}

void CloseGraphicsDevice(void)
{
    if (depthRenderbuffer) { glDeleteRenderbuffers(1, &depthRenderbuffer); depthRenderbuffer = 0; }
    if (colorRenderbuffer) { glDeleteRenderbuffers(1, &colorRenderbuffer); colorRenderbuffer = 0; }
    if (screenFramebuffer) { glDeleteFramebuffers(1, &screenFramebuffer); screenFramebuffer = 0; }
    if (iosEaglContext) { if ([EAGLContext currentContext] == iosEaglContext) [EAGLContext setCurrentContext:nil]; [iosEaglContext release]; iosEaglContext = NULL; }
    TRACELOG(LOG_INFO, "PLATFORM: IOS: Closed graphics device");
}

void MakeContextCurrent(void)
{
    if (iosEaglContext) { if (![EAGLContext setCurrentContext:iosEaglContext]) TRACELOG(LOG_ERROR, "PLATFORM: IOS: Failed to set current EAGLContext"); }
}

//----------------------------------------------------------------------------------
// Screen drawing functions
//----------------------------------------------------------------------------------
void BeginDrawing(void)
{
    CORE.Window.flags |= FLAG_APP_DRAW_PENDING; // Signal that drawing has started (if this flag is used by game loop)
}

void EndDrawing(void)
{
    rlglDraw();             // Process all batched draw calls (if any)
    SwapScreenBuffer();     // Present the frame
    CORE.Window.flags &= ~FLAG_APP_DRAW_PENDING; // Signal that drawing has ended (if this flag is used)
}

void SwapScreenBuffer(void)
{
    if (iosEaglContext && colorRenderbuffer) // colorRenderbuffer should be checked
    {
        glBindRenderbuffer(GL_RENDERBUFFER, colorRenderbuffer); // Ensure correct buffer is bound for presentation
        if (![iosEaglContext presentRenderbuffer:GL_RENDERBUFFER])
        {
            TRACELOG(LOG_ERROR, "PLATFORM: IOS: Failed to present renderbuffer. GL Error: 0x%X", glGetError());
        }
    } else {
        TRACELOG(LOG_WARNING, "PLATFORM: IOS: SwapScreenBuffer called with uninitialized EAGL context or renderbuffer.");
    }
}

//----------------------------------------------------------------------------------
// Window-related functions
//----------------------------------------------------------------------------------
bool WindowShouldClose(void) { return CORE.Window.shouldClose; }
void ToggleFullscreen(void) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: ToggleFullscreen() not applicable"); }
void ToggleBorderlessWindowed(void) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: ToggleBorderlessWindowed() not applicable"); }
void MaximizeWindow(void) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: MaximizeWindow() not applicable"); }
void MinimizeWindow(void) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: MinimizeWindow() not applicable"); }
void RestoreWindow(void) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: RestoreWindow() not applicable"); }

void SetWindowState(unsigned int flags) { TRACELOG(LOG_INFO, "PLATFORM: IOS: SetWindowState() called with flags: %u", flags); }
void ClearWindowState(unsigned int flags) { TRACELOG(LOG_INFO, "PLATFORM: IOS: ClearWindowState() called with flags: %u", flags); }

void SetWindowIcon(Image image) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: SetWindowIcon() not implemented (set via Xcode project settings)"); }
void SetWindowIcons(Image *images, int count) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: SetWindowIcons() not implemented (set via Xcode project settings)"); }
void SetWindowTitle(const char *title) { TRACELOG(LOG_INFO, "PLATFORM: IOS: SetWindowTitle() called, but title is usually set via Info.plist or project settings"); CORE.Window.title = title; }
void SetWindowPosition(int x, int y) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: SetWindowPosition() not applicable"); }
void SetWindowMonitor(int monitor) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: SetWindowMonitor() not applicable"); }
void SetWindowMinSize(int width, int height) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: SetWindowMinSize() not applicable"); }
void SetWindowMaxSize(int width, int height) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: SetWindowMaxSize() not applicable"); }
void SetWindowSize(int width, int height) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: SetWindowSize() not applicable (controlled by device screen or view controller)"); }
void SetWindowOpacity(float opacity) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: SetWindowOpacity() not implemented"); }
void SetWindowFocused(void) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: SetWindowFocused() not applicable"); }
void *GetWindowHandle(void) { return (void *)iosWindow; }

int GetScreenWidth(void) { return currentScreenWidth; }
int GetScreenHeight(void) { return currentScreenHeight; }

Vector2 GetWindowPosition(void) { return (Vector2){ 0.0f, 0.0f }; }
Vector2 GetWindowScaleDPI(void) { return (Vector2){ (float)[[UIScreen mainScreen] scale], (float)[[UIScreen mainScreen] scale] }; }

int GetMonitorCount(void) { return 1; }
int GetCurrentMonitor(void) { return 0; }
Vector2 GetMonitorPosition(int monitor) { return (Vector2){ 0.0f, 0.0f }; }
int GetMonitorWidth(int monitor) { if (monitor == 0) return (int)[[UIScreen mainScreen] nativeBounds].size.width; return 0; }
int GetMonitorHeight(int monitor) { if (monitor == 0) return (int)[[UIScreen mainScreen] nativeBounds].size.height; return 0; }
int GetMonitorPhysicalWidth(int monitor) { return 0; }
int GetMonitorPhysicalHeight(int monitor) { return 0; }
int GetMonitorRefreshRate(int monitor) { return 60; } // Typical iOS display refresh rate
const char *GetMonitorName(int monitor) { if (monitor == 0) return "Primary Display"; return ""; }

void SetClipboardText(const char *text)
{
    if (text != NULL) [[UIPasteboard generalPasteboard] setString:[NSString stringWithUTF8String:text]];
}

const char *GetClipboardText(void)
{
    static char clipboardText[1024];
    NSString *pasteboardString = [[UIPasteboard generalPasteboard] string];
    if (pasteboardString)
    {
        strncpy(clipboardText, [pasteboardString UTF8String], sizeof(clipboardText) - 1);
        clipboardText[sizeof(clipboardText) - 1] = '\0'; // Changed from ' ' to '\0'
        return clipboardText;
    }
    return NULL;
}

void ShowCursor(void) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: ShowCursor() not applicable"); }
void HideCursor(void) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: HideCursor() not applicable"); }
bool IsCursorHidden(void) { return true; }
void EnableCursor(void) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: EnableCursor() not applicable"); }
void DisableCursor(void) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: DisableCursor() not applicable"); }
bool IsCursorOnScreen(void) { return true; }

double GetTime(void) { return [[NSProcessInfo processInfo] systemUptime]; }

void OpenURL(const char *url)
{
    if (url == NULL) return;
    NSString *urlString = [NSString stringWithUTF8String:url];
    NSURL *nsURL = [NSURL URLWithString:urlString];
    if (nsURL && [[UIApplication sharedApplication] canOpenURL:nsURL])
    {
        [[UIApplication sharedApplication] openURL:nsURL options:@{} completionHandler:nil];
    }
    else
    {
        TRACELOG(LOG_WARNING, "PLATFORM: IOS: Failed to open URL: %s", url);
    }
}

// Input-related functions (Keyboard, Mouse, Gamepad, Touch)
// Keyboard functions are mostly stubs as physical keyboard is not standard on iOS
void SetKeyboardExitKey(int key) { CORE.Input.Keyboard.exitKey = key; }
int GetKeyPressed(void) { return CORE.Input.Keyboard.keyPressedQueue[0]; } // Basic support, no queue processing here
int GetCharPressed(void) { return CORE.Input.Keyboard.charPressedQueue[0]; } // Basic support
bool IsKeyPressed(int key) { return false; } // Requires actual keyboard event handling
bool IsKeyDown(int key) { return false; }
bool IsKeyReleased(int key) { return false; }
bool IsKeyUp(int key) { return true; } // Default to up

// Mouse functions are mapped to touch
bool IsMouseButtonPressed(int button) { if (button == MOUSE_BUTTON_LEFT) return (CORE.Input.Mouse.currentButtonState[button] == 1 && CORE.Input.Mouse.previousButtonState[button] == 0); return false; }
bool IsMouseButtonDown(int button) { if (button == MOUSE_BUTTON_LEFT) return (CORE.Input.Mouse.currentButtonState[button] == 1); return false; }
bool IsMouseButtonReleased(int button) { if (button == MOUSE_BUTTON_LEFT) return (CORE.Input.Mouse.currentButtonState[button] == 0 && CORE.Input.Mouse.previousButtonState[button] == 1); return false;}
bool IsMouseButtonUp(int button) { if (button == MOUSE_BUTTON_LEFT) return (CORE.Input.Mouse.currentButtonState[button] == 0); return true;} // Default to up if not left button
int GetMouseX(void) { return (int)CORE.Input.Mouse.currentPosition.x; }
int GetMouseY(void) { return (int)CORE.Input.Mouse.currentPosition.y; }
Vector2 GetMousePosition(void) { return CORE.Input.Mouse.currentPosition; }
Vector2 GetMouseDelta(void) { return (Vector2){ CORE.Input.Mouse.currentPosition.x - CORE.Input.Mouse.previousPosition.x, CORE.Input.Mouse.currentPosition.y - CORE.Input.Mouse.previousPosition.y }; }
void SetMousePosition(int x, int y) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: SetMousePosition() not applicable"); }
void SetMouseOffset(int offsetX, int offsetY) { CORE.Input.Mouse.offset = (Vector2){ (float)offsetX, (float)offsetY }; }
void SetMouseScale(float scaleX, float scaleY) { CORE.Input.Mouse.scale = (Vector2){ scaleX, scaleY }; }
float GetMouseWheelMove(void) { return CORE.Input.Mouse.currentWheelMove.y; }
Vector2 GetMouseWheelMoveV(void) { return CORE.Input.Mouse.currentWheelMove; }
void SetMouseCursor(int cursor) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: SetMouseCursor() not applicable"); }

// Touch functions
int GetTouchX(void) { if (CORE.Input.Touch.pointCount > 0) return (int)CORE.Input.Touch.position[0].x; return GetMouseX(); } // Fallback to mouse if no touch
int GetTouchY(void) { if (CORE.Input.Touch.pointCount > 0) return (int)CORE.Input.Touch.position[0].y; return GetMouseY(); }
Vector2 GetTouchPosition(int index) { if (index < MAX_TOUCH_POINTS && CORE.Input.Touch.pointId[index] != 0) return CORE.Input.Touch.position[index]; return CORE.Input.Mouse.currentPosition; }
int GetTouchPointId(int index) { if (index < MAX_TOUCH_POINTS) return CORE.Input.Touch.pointId[index]; return -1; }
int GetTouchPointCount(void) { return CORE.Input.Touch.pointCount; }

// Gamepad functions are stubs
bool IsGamepadAvailable(int gamepad) { return false; }
const char *GetGamepadName(int gamepad) { return NULL; }
bool IsGamepadButtonPressed(int gamepad, int button) { return false; }
bool IsGamepadButtonDown(int gamepad, int button) { return false; }
bool IsGamepadButtonReleased(int gamepad, int button) { return false; }
bool IsGamepadButtonUp(int gamepad, int button) { return true; }
int GetGamepadButtonPressed(void) { return 0; }
int GetGamepadAxisCount(int gamepad) { return 0; }
float GetGamepadAxisMovement(int gamepad, int axis) { return 0.0f; }
int SetGamepadMappings(const char *mappings) { TRACELOG(LOG_WARNING, "PLATFORM: IOS: SetGamepadMappings() not implemented"); return 0; }

void PollInputEvents(void)
{
#if defined(SUPPORT_GESTURES_SYSTEM) // Gestures are processed from touch events directly on iOS if enabled
    // Gestures are usually updated from touch data.
    // If SUPPORT_GESTURES_SYSTEM is high-level, it might be called from rcore.c's main loop.
    // For platform-specific gesture polling (if any), it would go here.
    // UpdateGestures(); // This is a generic call, might need platform specifics if any.
#endif

    // Reset keyboard state for next frame (queues are usually processed in rcore.c or user code)
    CORE.Input.Keyboard.keyPressedQueueCount = 0;
    CORE.Input.Keyboard.charPressedQueueCount = 0;
    // for (int i = 0; i < MAX_KEYBOARD_KEYS; i++) CORE.Input.Keyboard.keyRepeatInFrame[i] = 0; // If tracking key repeats

    // Update previous mouse state from current state
    CORE.Input.Mouse.previousPosition = CORE.Input.Mouse.currentPosition;
    for (int i = 0; i < MAX_MOUSE_BUTTONS; i++) CORE.Input.Mouse.previousButtonState[i] = CORE.Input.Mouse.currentButtonState[i];
    CORE.Input.Mouse.previousWheelMove = CORE.Input.Mouse.currentWheelMove;
    CORE.Input.Mouse.currentWheelMove = (Vector2){ 0.0f, 0.0f }; // Reset wheel move for current frame

    // Update previous touch state from current state
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
        CORE.Input.Touch.previousTouchState[i] = CORE.Input.Touch.currentTouchState[i];
        // CORE.Input.Touch.pointUpdated[i] = false; // Reset update flag if used
    }
}

#endif // PLATFORM_IOS
