// Small Cocoa helpers for the plug-in window (macOS only).
#import <Cocoa/Cocoa.h>

// Closes the host's window that holds the Sudz editor, the same as clicking its close button.
void sudzCloseHostWindow (void* viewHandle)
{
    NSView* view = (NSView*) viewHandle;
    if (view == nil) return;
    NSWindow* window = [view window];
    if (window == nil) return;
    [window retain];
    dispatch_async (dispatch_get_main_queue(), ^{
        if ([window styleMask] & NSWindowStyleMaskClosable) [window performClose: nil];
        else [window orderOut: nil];
        [window release];
    });
}
