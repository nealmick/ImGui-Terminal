/* See LICENSE for license details. */
/*
	platform/OSX/main.mm — native macOS shell using Cocoa + Metal.
*/

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "imgui.h"
#include "imgui_impl_osx.h"
#include "imgui_impl_metal.h"

#include "imgui_terminal.h"

static Terminal g_term;


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
	ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);

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

	self.view = view;
}

- (void)viewDidLoad
{
	[super viewDidLoad];

	MTKView *mtkView = (MTKView *) self.view;
	mtkView.delegate = self;
	
	ImGui_ImplOSX_Init(self.view);
	g_term.init(80, 24, NULL);
	g_term.set_retained(true);
	g_term.set_transparent(true);
}

- (void)viewWillAppear
{
	[super viewWillAppear];
	self.view.window.delegate = self;
}

- (void)windowWillClose:(NSNotification *)notification
{
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		g_term.shutdown();
	});
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

		if (passDesc == nil)
		{
			[cmdBuf commit];
			return;
		}

		ImGui_ImplMetal_NewFrame(passDesc);
		ImGui_ImplOSX_NewFrame(view);
		ImGui::NewFrame();

		ImGuiViewport *vp = ImGui::GetMainViewport();
		const float pad = 8.0f;
		const float title_height = 38.0f;
		ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + pad, vp->WorkPos.y + title_height));
		ImGui::SetNextWindowSize(
		    ImVec2(vp->WorkSize.x - pad * 2.0f,
			   vp->WorkSize.y - title_height - pad));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
		ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
					 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
					 ImGuiWindowFlags_NoSavedSettings;

		bool drew = false;
		if (ImGui::Begin("##term_host", NULL, flags))
		{
			ImGui::SetKeyboardFocusHere();
			drew = g_term.draw_canvas();
			if (!g_term.is_alive())
				[NSApp terminate:nil];
		}
		ImGui::End();
		ImGui::PopStyleVar(3);

		ImGui::Render();

		if (drew)
		{
			id<MTLRenderCommandEncoder> enc =
			    [cmdBuf renderCommandEncoderWithDescriptor:passDesc];
			ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cmdBuf, enc);
			[enc endEncoding];
			[cmdBuf presentDrawable:view.currentDrawable];
		}
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
				   NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable |
				   NSWindowStyleMaskFullSizeContentView;

		_window = [[NSWindow alloc] initWithContentRect:NSZeroRect
						      styleMask:style
							backing:NSBackingStoreBuffered
							  defer:NO];
		_window.titlebarAppearsTransparent = YES;
		_window.titleVisibility = NSWindowTitleHidden;
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
