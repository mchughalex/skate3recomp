#include <filesystem>
#include <string>

#include <SDL3/SDL.h>

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace skate3 {

std::filesystem::path PickTitleUpdateFileMacOS() {
  @autoreleasepool {
    SDL_Window* sdl_window = SDL_GetKeyboardFocus();
    NSWindow* ns_window = nil;

    if (sdl_window) {
      SDL_PropertiesID properties = SDL_GetWindowProperties(sdl_window);
      ns_window = static_cast<NSWindow*>(
          SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr));
      SDL_RaiseWindow(sdl_window);
    }

    [NSApp activateIgnoringOtherApps:YES];
    if (ns_window) {
      [ns_window makeKeyAndOrderFront:nil];
    }

    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setTitle:@"Select the Skate 3 Title Update 4 package"];
    [panel setPrompt:@"Select"];
    [panel setCanChooseFiles:YES];
    [panel setCanChooseDirectories:NO];
    [panel setAllowsMultipleSelection:NO];
    [panel setResolvesAliases:YES];
    [panel setAllowsOtherFileTypes:YES];

    NSInteger response = [panel runModal];
    std::filesystem::path result;
    if (response == NSModalResponseOK) {
      NSURL* url = [[panel URLs] firstObject];
      if (url && [url isFileURL]) {
        result = std::string([[url path] UTF8String]);
      }
    }

    return result;
  }
}

std::filesystem::path PickIsoFileMacOS() {
  @autoreleasepool {
    SDL_Window* sdl_window = SDL_GetKeyboardFocus();
    NSWindow* ns_window = nil;

    if (sdl_window) {
      SDL_PropertiesID properties = SDL_GetWindowProperties(sdl_window);
      ns_window = static_cast<NSWindow*>(
          SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr));
      SDL_RaiseWindow(sdl_window);
    }

    [NSApp activateIgnoringOtherApps:YES];
    if (ns_window) {
      [ns_window makeKeyAndOrderFront:nil];
    }

    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setTitle:@"Select Skate 3 Xbox 360 ISO"];
    [panel setPrompt:@"Select ISO"];
    [panel setCanChooseFiles:YES];
    [panel setCanChooseDirectories:NO];
    [panel setAllowsMultipleSelection:NO];
    [panel setResolvesAliases:YES];
    UTType* iso_type = [UTType typeWithFilenameExtension:@"iso"];
    if (iso_type) {
      [panel setAllowedContentTypes:@[ iso_type ]];
    }

    NSInteger response = [panel runModal];
    std::filesystem::path result;
    if (response == NSModalResponseOK) {
      NSURL* url = [[panel URLs] firstObject];
      if (url && [url isFileURL]) {
        result = std::string([[url path] UTF8String]);
      }
    }

    return result;
  }
}

}  // namespace skate3
