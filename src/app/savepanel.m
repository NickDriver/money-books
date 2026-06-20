/* NSSavePanel-backed implementation of mb_save_panel (see savepanel.h). */
#import <Cocoa/Cocoa.h>
#include <string.h>
#include <stdio.h>
#include "savepanel.h"

int mb_save_panel(const char *suggested, const char *content, char *out, size_t out_n) {
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
