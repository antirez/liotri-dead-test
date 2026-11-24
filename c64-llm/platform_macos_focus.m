#import <AppKit/AppKit.h>

void c64_llm_macos_activate_app(void) {
    [NSApplication sharedApplication];
    [NSApp activateIgnoringOtherApps:YES];
}
