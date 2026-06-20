/*
 * macOS implementation of the desktop-shell platform shim (see platform.h).
 * Cocoa, dyld, and the BSD path helpers live here so main.c stays platform-free.
 */
#import <Cocoa/Cocoa.h>
#include <mach-o/dyld.h>   /* _NSGetExecutablePath — locate the sibling MCP binary */
#include <libgen.h>        /* dirname */
#include <limits.h>        /* PATH_MAX */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "platform.h"

int mb_platform_mcp_binary_path(char *out, size_t out_n) {
  char exe[PATH_MAX]; uint32_t sz = sizeof exe;
  if (_NSGetExecutablePath(exe, &sz) != 0) { snprintf(out, out_n, "money-books-mcp"); return -1; }
  char real[PATH_MAX], tmp[PATH_MAX];
  snprintf(tmp, sizeof tmp, "%s", realpath(exe, real) ? real : exe);
  snprintf(out, out_n, "%s/money-books-mcp", dirname(tmp));
  return 0;
}

int mb_platform_claude_config_path(char *out, size_t out_n) {
  const char *home = getenv("HOME");
  if (!home || !home[0]) {
    snprintf(out, out_n, "~/Library/Application Support/Claude/claude_desktop_config.json");
    return -1;
  }
  snprintf(out, out_n, "%s/Library/Application Support/Claude/claude_desktop_config.json", home);
  return 0;
}

int mb_platform_save_file(const char *suggested, const char *content, char *out, size_t out_n) {
  if (out && out_n) out[0] = '\0';
  @autoreleasepool {
    NSSavePanel *panel = [NSSavePanel savePanel];
    if (suggested && suggested[0])
      panel.nameFieldStringValue = [NSString stringWithUTF8String:suggested];
    panel.canCreateDirectories = YES;

    if ([panel runModal] != NSModalResponseOK) return 0;  /* cancelled */

    NSURL *url = panel.URL;
    if (!url) { snprintf(out, out_n, "no file selected"); return -1; }

    NSData *data = [NSData dataWithBytes:(content ? content : "")
                                 length:(content ? strlen(content) : 0)];
    NSError *err = nil;
    if (![data writeToURL:url options:NSDataWritingAtomic error:&err]) {
      snprintf(out, out_n, "%s",
               err ? err.localizedDescription.UTF8String : "could not write file");
      return -1;
    }
    snprintf(out, out_n, "%s", url.path.UTF8String);
    return 1;
  }
}
