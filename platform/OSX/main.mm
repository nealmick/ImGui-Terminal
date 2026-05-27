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

	  Build: `make p_osx` from the repo root  →  build/p_osx
*/

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "imgui.h"
#include "imgui_impl_osx.h"
#include "imgui_impl_metal.h"

#include "imgui_terminal.h"

/*
	The Terminal instance is owned at file scope so the view-controller,
	menu actions, and the run-loop teardown all see the same object. One
	terminal per native client — no point hiding it inside @property.
*/
static Terminal g_term;

/*
	View controller wraps the MTKView and lets MTKView's CADisplayLink
	drive frames via -drawInMTKView:. Conforms to MTKViewDelegate
	(per-frame draw) and NSWindowDelegate (clean shutdown on close).
*/
@interface AppViewController : NSViewController <MTKViewDelegate, NSWindowDelegate>
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> commandQueue;
@end

@implementation AppViewController

- (instancetype)init
{
	self = [super initWithNibName:nil bundle:nil];

	_device = MTLCreateSystemDefaultDevice();
	_commandQueue = [_device newCommandQueue];
	if (!_device)
	{
		NSLog(@"Metal is not supported on this device");
		abort();
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();

	ImGui::StyleColorsDark();

	/* Retina / HiDPI font density tracking — see main_example_glfw_gl.cpp
	   for rationale. The Metal renderer backend honors RendererHasTextures. */
	io.ConfigDpiScaleFonts = true;

	ImGui_ImplMetal_Init(_device);

	return self;
}

- (void)loadView
{
	MTKView *view = [[MTKView alloc] initWithFrame:CGRectMake(0, 0, 1200, 800) device:_device];
	view.clearColor = MTLClearColorMake(0.05, 0.05, 0.05, 1.0);
	const char *home = getenv("HOME");
	if (home)
	{
		chdir(home);
	}

	/* Force the app bundle to know about Homebrew and standard local bins */
	const char *current_path = getenv("PATH");
	NSString *new_path = [NSString stringWithFormat:@"/opt/homebrew/bin:/usr/local/bin:%s",
	    current_path ? current_path : "/usr/bin:/bin"];
	setenv("PATH", [new_path UTF8String], 1);
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

	MTKView *mtkView = (MTKView *) self.view;
	mtkView.delegate = self;

	/* Hand the platform backend the NSView; it installs an NSEvent
	   monitor on it for keyboard / mouse / IME plumbing. */
	ImGui_ImplOSX_Init(self.view);

	/* Spin up the terminal core (forks the PTY, default $SHELL with NULL
	   argv). 80x24 is just an initial cell grid — the widget resizes
	   itself to whatever pixel area Begin/End gives it. */
	g_term.init(80, 24, NULL);

	/* Default to transparent canvas on macOS — the MTKView clearColor
	   shows through, so the terminal blends with the window chrome.
	   Toggle from View → Enable/Disable Transparency. */
	g_term.set_transparent(true);
}

- (void)viewWillAppear
{
	[super viewWillAppear];
	self.view.window.delegate = self;
}

- (void)windowWillClose:(NSNotification *)notification
{
	g_term.shutdown();
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
	@autoreleasepool
	{
		ImGuiIO &io = ImGui::GetIO();
		io.DisplaySize.x = view.bounds.size.width;
		io.DisplaySize.y = view.bounds.size.height;

		CGFloat scale =
		    view.window.screen.backingScaleFactor ?: NSScreen.mainScreen.backingScaleFactor;
		io.DisplayFramebufferScale = ImVec2(scale, scale);

		id<MTLCommandBuffer> cmdBuf = [self.commandQueue commandBuffer];
		MTLRenderPassDescriptor *passDesc = view.currentRenderPassDescriptor;

		/* nil when occluded / off-screen — commit empty and skip. */
		if (passDesc == nil)
		{
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
		const float pad = 8.0f;
		ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + pad, vp->WorkPos.y + pad));
		ImGui::SetNextWindowSize(
		    ImVec2(vp->WorkSize.x - pad * 2.0f, vp->WorkSize.y - pad * 2.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
					 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
					 ImGuiWindowFlags_NoSavedSettings;
		if (ImGui::Begin("##term_host", NULL, flags))
		{
			ImGui::SetKeyboardFocusHere();
			g_term.draw_canvas();
			if (!g_term.is_alive())
				[NSApp terminate:nil];
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

@interface AppDelegate : NSObject <NSApplicationDelegate, NSMenuDelegate>
@property(nonatomic, strong) NSWindow *window;
@end

@implementation AppDelegate

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
	return YES;
}

/*
	Build the system menu bar (the strip at the very top of the screen
	with the Apple logo, app name, File, Edit, View, ...). We add two
	submenus:

	  - App menu  (first item; macOS substitutes the running app's name)
	      → Quit (⌘Q)
	  - View menu
	      → Transparent Background  (toggles term_set_transparent)

	Checkmark state for "Transparent Background" is updated at open time
	via -validateMenuItem: — that way it always reflects the live value
	of term_get_transparent() without us tracking it.
*/
- (void)installMainMenu
{
	NSMenu *mainMenu = [[NSMenu alloc] init];

	/* App menu */
	NSMenuItem *appItem = [[NSMenuItem alloc] init];
	[mainMenu addItem:appItem];
	NSMenu *appMenu = [[NSMenu alloc] init];
	[appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
	appItem.submenu = appMenu;

	/* View menu */
	NSMenuItem *viewItem = [[NSMenuItem alloc] init];
	[mainMenu addItem:viewItem];
	NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
	viewMenu.delegate = self;
	/*
		Show/Hide-style label instead of a checkmark — title flips
		between "Enable Transparency" and "Disable Transparency" in
		-menuNeedsUpdate:. Mirrors Finder's "Hide Toolbar/Show Toolbar"
		idiom; avoids the checkmark column entirely.
	*/
	NSMenuItem *transparent = [viewMenu addItemWithTitle:@"Enable Transparency"
						      action:@selector(toggleTransparent:)
					       keyEquivalent:@""];
	transparent.target = self;

	NSMenuItem *bigger = [viewMenu addItemWithTitle:@"Increase Font Size"
						 action:@selector(increaseFontSize:)
					  keyEquivalent:@"+"];
	bigger.target = self;
	NSMenuItem *smaller = [viewMenu addItemWithTitle:@"Decrease Font Size"
						  action:@selector(decreaseFontSize:)
					   keyEquivalent:@"-"];
	smaller.target = self;

	viewItem.submenu = viewMenu;

	NSApp.mainMenu = mainMenu;
}

- (instancetype)init
{
	if ((self = [super init]))
	{
		AppViewController *vc = [[AppViewController alloc] init];

		NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
				   NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable;

		_window = [[NSWindow alloc] initWithContentRect:NSZeroRect
						      styleMask:style
							backing:NSBackingStoreBuffered
							  defer:NO];
		_window.title = @"st-imgui (cocoa + metal)";
		_window.contentViewController = vc;
		[_window center];
		[_window makeKeyAndOrderFront:self];

		[self installMainMenu];
	}
	return self;
}

/* Menu actions -------------------------------------------------------- */

- (void)toggleTransparent:(NSMenuItem *)sender
{
	g_term.set_transparent(!g_term.is_transparent());
}

- (void)increaseFontSize:(NSMenuItem *)sender
{
	g_term.set_font_size(g_term.get_font_size() + 2.0f);
}

- (void)decreaseFontSize:(NSMenuItem *)sender
{
	g_term.set_font_size(g_term.get_font_size() - 2.0f);
}

/*
	NSMenuDelegate — fires right before the menu is sized and shown.
	Flip the title to reflect the action that clicking will perform
	("Enable" when currently off, "Disable" when currently on).
*/
- (void)menuNeedsUpdate:(NSMenu *)menu
{
	for (NSMenuItem *item in menu.itemArray)
	{
		if (item.action == @selector(toggleTransparent:))
		{
			item.title = g_term.is_transparent() ? @"Disable Transparency"
							     : @"Enable Transparency";
		}
	}
}

@end

/* main --------------------------------------------------------------- */

int
main(int, const char **)
{
	@autoreleasepool
	{
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
