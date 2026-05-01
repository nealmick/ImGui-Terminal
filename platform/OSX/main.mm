/* See LICENSE for license details. */
/*
	platform/OSX/main.mm — native macOS shell using Cocoa + Metal.

	No GLFW. Cocoa (NSApplication / NSWindow) owns windowing and input;
	Metal (via MTKView) owns rendering. ImGui's official osx + metal
	backends bridge the two — imgui_impl_osx.mm hooks NSEvent for
	keyboard / mouse, imgui_impl_metal.mm draws into the MTKView's
	current drawable each frame.

	macOS only. The .mm extension is required because Cocoa and Metal
	are Objective-C APIs.

	  Build: `make osx`   →   build/imgui_terminal_osx
*/

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "imgui.h"
#include "imgui_impl_osx.h"
#include "imgui_impl_metal.h"

/* Public API of imgui_win.cpp (the renderer-agnostic terminal widget).
   C++ linkage — defined as plain C++, not extern "C". term_draw_canvas
   renders into the *current* ImGui window (no Begin/End of its own), so
   this shell can pin it to the NSWindow with no chrome. Focus is
   standard ImGui IsItemFocused() on the canvas's InvisibleButton; the
   shell calls SetKeyboardFocusHere() each frame to keep that focus on
   the canvas (there's nothing else to focus in a pinned window). */
extern void term_init(int cols, int rows, char **argv);
extern void term_draw_canvas(void);
extern void term_shutdown(void);

/*
	View controller wraps the MTKView and lets MTKView's CADisplayLink
	drive frames via -drawInMTKView:. Conforms to MTKViewDelegate
	(per-frame draw) and NSWindowDelegate (clean shutdown on close).
*/
@interface AppViewController : NSViewController <MTKViewDelegate, NSWindowDelegate>
@property (nonatomic, strong) id<MTLDevice>       device;
@property (nonatomic, strong) id<MTLCommandQueue> commandQueue;
@end

@implementation AppViewController

- (instancetype)init
{
	self = [super initWithNibName:nil bundle:nil];

	_device       = MTLCreateSystemDefaultDevice();
	_commandQueue = [_device newCommandQueue];
	if (!_device) {
		NSLog(@"Metal is not supported on this device");
		abort();
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	ImGui::StyleColorsDark();

	/* Retina / HiDPI font density tracking — see main_example_glfw_gl.cpp
	   for rationale. The Metal renderer backend honors RendererHasTextures. */
	io.ConfigDpiScaleFonts = true;

	ImGui_ImplMetal_Init(_device);

	return self;
}

- (void)loadView
{
	MTKView *view = [[MTKView alloc] initWithFrame:CGRectMake(0, 0, 1200, 800)
	                                        device:_device];
	view.clearColor = MTLClearColorMake(0.05, 0.05, 0.05, 1.0);

	/* Pin the display link to the panel's refresh rate. Default of 60
	   makes ProMotion drop to its 48Hz adaptive bin; bumping past the
	   panel rate is a hint that gets clamped to whatever the panel
	   actually supports (144 / 120 / 60). */
	view.preferredFramesPerSecond = 240;

	self.view = view;
}

- (void)viewDidLoad
{
	[super viewDidLoad];

	MTKView *mtkView = (MTKView *)self.view;
	mtkView.delegate = self;

	/* Hand the platform backend the NSView; it installs an NSEvent
	   monitor on it for keyboard / mouse / IME plumbing. */
	ImGui_ImplOSX_Init(self.view);

	/* Spin up the terminal core (forks the PTY, default $SHELL with NULL
	   argv). 80x24 is just an initial cell grid — the widget resizes
	   itself to whatever pixel area Begin/End gives it. */
	term_init(80, 24, NULL);
}

- (void)viewWillAppear
{
	[super viewWillAppear];
	self.view.window.delegate = self;
}

- (void)windowWillClose:(NSNotification *)notification
{
	term_shutdown();
	ImGui_ImplMetal_Shutdown();
	ImGui_ImplOSX_Shutdown();
	ImGui::DestroyContext();
}

/* MTKViewDelegate ----------------------------------------------------- */

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size
{
	/* MTKView handles drawableSize itself; nothing to do here. */
}

- (void)drawInMTKView:(MTKView *)view
{
	@autoreleasepool {
		ImGuiIO &io = ImGui::GetIO();
		io.DisplaySize.x = view.bounds.size.width;
		io.DisplaySize.y = view.bounds.size.height;

		CGFloat scale = view.window.screen.backingScaleFactor
		                ?: NSScreen.mainScreen.backingScaleFactor;
		io.DisplayFramebufferScale = ImVec2(scale, scale);

		id<MTLCommandBuffer>     cmdBuf   = [self.commandQueue commandBuffer];
		MTLRenderPassDescriptor *passDesc = view.currentRenderPassDescriptor;

		/* nil when occluded / off-screen — commit empty and skip. */
		if (passDesc == nil) {
			[cmdBuf commit];
			return;
		}

		ImGui_ImplMetal_NewFrame(passDesc);
		ImGui_ImplOSX_NewFrame(view);
		ImGui::NewFrame();

		/* Pin the terminal canvas to the NSWindow's content area with
		   no ImGui chrome — the NSWindow itself still has the standard
		   traffic-light titlebar. SetKeyboardFocusHere targets the next
		   ImGui item submitted (the canvas's InvisibleButton); calling
		   it every frame keeps the canvas focused even after a
		   titlebar drag drops nav focus. There's nothing else to focus
		   in a pinned window, so this is idempotent. */
		ImGuiViewport *vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->WorkPos);
		ImGui::SetNextWindowSize(vp->WorkSize);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
		ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
		                       | ImGuiWindowFlags_NoResize
		                       | ImGuiWindowFlags_NoMove
		                       | ImGuiWindowFlags_NoCollapse
		                       | ImGuiWindowFlags_NoSavedSettings;
		if (ImGui::Begin("##term_host", NULL, flags)) {
			ImGui::SetKeyboardFocusHere();
			term_draw_canvas();
		}
		ImGui::End();
		ImGui::PopStyleVar(3);

		ImGui::Render();

		id<MTLRenderCommandEncoder> enc =
		    [cmdBuf renderCommandEncoderWithDescriptor:passDesc];
		ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cmdBuf, enc);
		[enc endEncoding];

		[cmdBuf presentDrawable:view.currentDrawable];
		[cmdBuf commit];
	}
}

@end

/* AppDelegate -------------------------------------------------------- */

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSWindow *window;
@end

@implementation AppDelegate

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
	return YES;
}

- (instancetype)init
{
	if ((self = [super init])) {
		AppViewController *vc = [[AppViewController alloc] init];

		NSUInteger style = NSWindowStyleMaskTitled
		                 | NSWindowStyleMaskClosable
		                 | NSWindowStyleMaskResizable
		                 | NSWindowStyleMaskMiniaturizable;

		_window = [[NSWindow alloc] initWithContentRect:NSZeroRect
		                                      styleMask:style
		                                        backing:NSBackingStoreBuffered
		                                          defer:NO];
		_window.title                 = @"st-imgui (cocoa + metal)";
		_window.contentViewController = vc;
		[_window center];
		[_window makeKeyAndOrderFront:self];
	}
	return self;
}

@end

/* main --------------------------------------------------------------- */

int
main(int, const char **)
{
	@autoreleasepool {
		/* Disable "Press and Hold" for this app to ensure standard key
		   repeats work (holding 'j' sends repeated 'j' chars instead
		   of showing the accent picker). Matches GLFW/SDL behavior. */
		[[NSUserDefaults standardUserDefaults] setBool:NO
		                                       forKey:@"ApplePressAndHoldEnabled"];

		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

		AppDelegate *delegate = [[AppDelegate alloc] init];
		[NSApp setDelegate:delegate];

		[NSApp activateIgnoringOtherApps:YES];
		[NSApp run];
	}
	return 0;
}
