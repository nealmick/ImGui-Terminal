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

#include "terminal.h"

static Terminal g_term;


@interface AppViewController : NSViewController <MTKViewDelegate, NSWindowDelegate>
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property(nonatomic, strong) dispatch_source_t ptyTimer;
@end

@interface AppViewController ()
@property(nonatomic, strong) MTKView *mtkView;
@property(nonatomic, strong) NSView *tintView;
- (void)setWindowTintColor:(NSColor *)color;
- (NSColor *)windowTintColor;
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
	NSVisualEffectView *blurView =
	    [[NSVisualEffectView alloc] initWithFrame:CGRectMake(0, 0, 1200, 800)];
	blurView.material = NSVisualEffectMaterialHUDWindow;
	blurView.blendingMode = NSVisualEffectBlendingModeBehindWindow;
	blurView.state = NSVisualEffectStateActive;
	blurView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

	MTKView *view = [[MTKView alloc] initWithFrame:blurView.bounds device:_device];
	view.clearColor = MTLClearColorMake(0, 0, 0, 0);
	view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	view.layer.opaque = NO;

	self.tintView = [[NSView alloc] initWithFrame:blurView.bounds];
	self.tintView.wantsLayer = YES;
	self.tintView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	self.tintView.layer.backgroundColor =
	    [NSColor colorWithSRGBRed:0.06 green:0.06 blue:0.10 alpha:0.35].CGColor;

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

	self.mtkView = view;
	[blurView addSubview:self.tintView];
	[blurView addSubview:view];
	self.view = blurView;
}

- (void)viewDidLoad
{
	[super viewDidLoad];

	MTKView *mtkView = (MTKView *) self.mtkView;
	mtkView.delegate = self;
	
	ImGui_ImplOSX_Init(mtkView);
	g_term.init(80, 24, NULL);
	g_term.set_retained(true);
	g_term.set_transparent(true);

	/* High-frequency PTY pump (~1000 Hz) — processes terminal data
	   between display frames so the next draw always sees up-to-date
	   terminal state instead of spreading work over multiple frames. */
	dispatch_source_t timer = dispatch_source_create(
	    DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, NSEC_PER_MSEC, 0);
	dispatch_source_set_event_handler(timer, ^{ g_term.tick(); });
	dispatch_resume(timer);
	self.ptyTimer = timer;
}

- (void)setWindowTintColor:(NSColor *)color
{
	NSColor *srgb = [color colorUsingColorSpace:NSColorSpace.sRGBColorSpace] ?: color;
	self.tintView.layer.backgroundColor = srgb.CGColor;
}

- (NSColor *)windowTintColor
{
	CGColorRef cg = self.tintView.layer.backgroundColor;
	if (!cg)
		return [NSColor clearColor];
	return [NSColor colorWithCGColor:cg];
}

- (void)viewWillAppear
{
	[super viewWillAppear];
	self.view.window.delegate = self;
}

- (void)windowWillClose:(NSNotification *)notification
{
	if (self.ptyTimer)
	{
		dispatch_source_cancel(self.ptyTimer);
		self.ptyTimer = nil;
	}
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

@interface AppDelegate ()
@property(nonatomic, strong) NSWindow *settingsWindow;
@property(nonatomic, strong) NSColorWell *colorWell;
@property(nonatomic, strong) NSButton *transparencyCheckbox;
@property(nonatomic, strong) NSSlider *fontSizeSlider;
@property(nonatomic, strong) NSTextField *fontSizeValueLabel;
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
	NSMenuItem *settingsItem =
	    [appMenu addItemWithTitle:@"Settings…" action:@selector(showSettings:) keyEquivalent:@","];
	settingsItem.target = self;
	[appMenu addItem:[NSMenuItem separatorItem]];
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
		_window.opaque = NO;
		_window.backgroundColor = [NSColor clearColor];
		_window.hasShadow = YES;
		_window.title = @"ImguiTerminal";
		_window.contentViewController = vc;
		[_window center];
		[_window makeKeyAndOrderFront:self];

		[self installMainMenu];
		[self loadSettings];
	}
	return self;
}

/* Menu actions -------------------------------------------------------- */

- (void)showSettings:(id)sender
{
	if (!self.settingsWindow)
	{
		NSUInteger settingsStyle = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable;
		self.settingsWindow =
		    [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 360, 190)
					      styleMask:settingsStyle
						backing:NSBackingStoreBuffered
						  defer:NO];
		[self.settingsWindow setTitle:@"Settings"];
		[self.settingsWindow center];
		self.settingsWindow.releasedWhenClosed = NO;
		NSView *content = self.settingsWindow.contentView;

		/* Window background color picker */
		NSTextField *colorLabel = [NSTextField labelWithString:@"Window Background:"];
		colorLabel.frame = NSMakeRect(20, 145, 150, 20);
		[content addSubview:colorLabel];

		self.colorWell = [[NSColorWell alloc] initWithFrame:NSMakeRect(190, 138, 140, 30)];
		self.colorWell.target = self;
		self.colorWell.action = @selector(colorChanged:);
		[content addSubview:self.colorWell];

		/* Buffer transparency toggle */
		self.transparencyCheckbox =
		    [[NSButton alloc] initWithFrame:NSMakeRect(20, 105, 200, 22)];
		[self.transparencyCheckbox setButtonType:NSButtonTypeSwitch];
		[self.transparencyCheckbox setTitle:@"Buffer Transparency"];
		[self.transparencyCheckbox setTarget:self];
		[self.transparencyCheckbox setAction:@selector(toggleBufferTransparency:)];
		[content addSubview:self.transparencyCheckbox];

		/* Font size slider */
		NSTextField *fontSizeLabel = [NSTextField labelWithString:@"Font Size:"];
		fontSizeLabel.frame = NSMakeRect(20, 65, 70, 20);
		[content addSubview:fontSizeLabel];

		self.fontSizeSlider = [[NSSlider alloc] initWithFrame:NSMakeRect(95, 65, 180, 20)];
		self.fontSizeSlider.minValue = 8.0;
		self.fontSizeSlider.maxValue = 40.0;
		self.fontSizeSlider.continuous = YES;
		self.fontSizeSlider.target = self;
		self.fontSizeSlider.action = @selector(fontSizeChanged:);
		[content addSubview:self.fontSizeSlider];

		self.fontSizeValueLabel = [NSTextField labelWithString:@"14"];
		self.fontSizeValueLabel.frame = NSMakeRect(285, 65, 55, 20);
		self.fontSizeValueLabel.alignment = NSTextAlignmentRight;
		[content addSubview:self.fontSizeValueLabel];
	}
	[self refreshSettingsControls];
	[self.settingsWindow makeKeyAndOrderFront:self];
	[NSApp activateIgnoringOtherApps:YES];
}

- (void)refreshSettingsControls
{
	AppViewController *vc = (AppViewController *) self.window.contentViewController;
	[self.colorWell setColor:[vc windowTintColor]];
	[self.transparencyCheckbox setState:g_term.is_transparent()
					 ? NSControlStateValueOn
					 : NSControlStateValueOff];
	float currentSize = g_term.get_font_size();
	[self.fontSizeSlider setFloatValue:currentSize];
	[self.fontSizeValueLabel setStringValue:[NSString stringWithFormat:@"%.0f", currentSize]];
}

- (NSString *)settingsFilePath
{
	NSFileManager *fm = [NSFileManager defaultManager];
	NSURL *appSupport =
	    [fm URLForDirectory:NSApplicationSupportDirectory
		     inDomain:NSUserDomainMask
	    appropriateForURL:nil
		       create:YES
			error:nil];
	NSURL *appDir = [appSupport URLByAppendingPathComponent:@"com.imgui-terminal"
						   isDirectory:YES];
	[fm createDirectoryAtURL:appDir
	    withIntermediateDirectories:YES
			     attributes:nil
				  error:nil];
	return [[appDir URLByAppendingPathComponent:@"settings.json"] path];
}

- (void)saveSettings
{
	AppViewController *vc = (AppViewController *) self.window.contentViewController;
	NSColor *color = [vc windowTintColor];
	CGFloat r, g, b, a;
	NSColor *srgb = [color colorUsingColorSpace:NSColorSpace.sRGBColorSpace] ?: color;
	[srgb getRed:&r green:&g blue:&b alpha:&a];

	NSDictionary *dict = @{
	    @"tint_color" : @{@"r" : @(r), @"g" : @(g), @"b" : @(b), @"a" : @(a)},
	    @"buffer_transparency" : @(g_term.is_transparent()),
	    @"font_size" : @(g_term.get_font_size()),
	};

	NSString *path = [self settingsFilePath];
	NSError *error = nil;
	NSData *data = [NSJSONSerialization dataWithJSONObject:dict
						       options:NSJSONWritingPrettyPrinted
							 error:&error];
	if (data)
		[data writeToFile:path atomically:YES];
}

- (void)loadSettings
{
	NSString *path = [self settingsFilePath];
	NSData *data = [NSData dataWithContentsOfFile:path];
	if (!data)
		return;

	NSDictionary *dict = [NSJSONSerialization JSONObjectWithData:data
							     options:0
							       error:nil];
	if (![dict isKindOfClass:[NSDictionary class]])
		return;

	/* Load tint color */
	NSDictionary *tint = dict[@"tint_color"];
	if ([tint isKindOfClass:[NSDictionary class]])
	{
		CGFloat r = [tint[@"r"] doubleValue];
		CGFloat g = [tint[@"g"] doubleValue];
		CGFloat b = [tint[@"b"] doubleValue];
		CGFloat a = [tint[@"a"] doubleValue];
		NSColor *color = [NSColor colorWithSRGBRed:r green:g blue:b alpha:a];
		AppViewController *vc = (AppViewController *) self.window.contentViewController;
		[vc setWindowTintColor:color];
	}

	/* Load buffer transparency */
	NSNumber *transparent = dict[@"buffer_transparency"];
	if (transparent)
		g_term.set_transparent([transparent boolValue]);

	/* Load font size */
	NSNumber *fontSize = dict[@"font_size"];
	if (fontSize)
		g_term.set_font_size([fontSize floatValue]);
}

- (void)colorChanged:(NSColorWell *)sender
{
	AppViewController *vc = (AppViewController *) self.window.contentViewController;
	[vc setWindowTintColor:[sender color]];
	[self saveSettings];
}

- (void)toggleBufferTransparency:(NSButton *)sender
{
	g_term.set_transparent([sender state] == NSControlStateValueOn);
	[self saveSettings];
}

- (void)fontSizeChanged:(NSSlider *)sender
{
	float size = [sender floatValue];
	g_term.set_font_size(size);
	[self.fontSizeValueLabel setStringValue:[NSString stringWithFormat:@"%.0f", size]];
	[self saveSettings];
}

- (void)toggleTransparent:(NSMenuItem *)sender
{
	g_term.set_transparent(!g_term.is_transparent());
	[self saveSettings];
}

- (void)increaseFontSize:(NSMenuItem *)sender
{
	g_term.set_font_size(g_term.get_font_size() + 2.0f);
	[self saveSettings];
}

- (void)decreaseFontSize:(NSMenuItem *)sender
{
	g_term.set_font_size(g_term.get_font_size() - 2.0f);
	[self saveSettings];
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
