/*
 * it_dialog_mac.c
 * ---------------
 * Feature 016 (issue #26): the macOS system file dialogs for the SDL
 * backend -- NSOpenPanel / NSSavePanel, driven from plain C through the
 * Objective-C runtime (as it_screen_sdl.c already does for the
 * press-and-hold setting), so the port stays C11 with no .m sources.
 * AppKit is already loaded in the process by SDL; CoreFoundation and
 * libobjc are linked (CMakeLists.txt). CF objects are toll-free bridged
 * to their NS counterparts (CFString/NSString, CFArray/NSArray,
 * CFURL/NSURL).
 *
 * Paths come back as UTF-8, which the C library takes unchanged; the
 * display string is derived with Screen_Utf8ToCP437Display.
 */

#if defined(__APPLE__) && defined(HAVE_SDL)

#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include "it_screen.h"

/* libobjc exports these; the public headers do not declare them */
extern void *objc_autoreleasePoolPush(void);
extern void  objc_autoreleasePoolPop(void *pool);

#define SEL_(n) sel_registerName(n)

static id msg0(id o, const char *s)
{
    return ((id (*)(id, SEL))objc_msgSend)(o, SEL_(s));
}

static void msg_id(id o, const char *s, id a)
{
    ((void (*)(id, SEL, id))objc_msgSend)(o, SEL_(s), a);
}

static void msg_bool(id o, const char *s, BOOL b)
{
    ((void (*)(id, SEL, BOOL))objc_msgSend)(o, SEL_(s), b);
}

/* CF objects created here are released by the caller via CFRelease */
static CFStringRef cfstr(const char *s)
{
    return CFStringCreateWithCString(NULL, s ? s : "", kCFStringEncodingUTF8);
}

/* "it;s3m" -> NSArray of NSString */
static CFArrayRef ext_array(const char *exts)
{
    const void *items[32];
    int n = 0;
    char buf[16];
    const char *p = exts;
    CFArrayRef a;
    int i;

    while (*p && n < 32) {
        size_t k = 0;
        while (*p && *p != ';' && k < sizeof(buf) - 1)
            buf[k++] = *p++;
        buf[k] = 0;
        if (*p == ';')
            p++;
        if (k)
            items[n++] = cfstr(buf);
    }
    a = CFArrayCreate(NULL, items, n, &kCFTypeArrayCallBacks);
    for (i = 0; i < n; i++)
        CFRelease(items[i]);
    return a;
}

static void set_start_dir(id panel, const char *dir)
{
    CFStringRef s;
    CFURLRef url;

    if (!dir || !*dir)
        return;
    s = cfstr(dir);
    url = CFURLCreateWithFileSystemPath(NULL, s, kCFURLPOSIXPathStyle, true);
    if (url) {
        msg_id(panel, "setDirectoryURL:", (id)url);
        CFRelease(url);
    }
    CFRelease(s);
}

int Dialog_Mac(const it_dialog_req_t *req, it_dialog_res_t *res)
{
    int save = req->kind == IT_DLG_SAVE_MODULE;
    int folder = req->kind == IT_DLG_PICK_FOLDER;
    void *pool = objc_autoreleasePoolPush();
    Class cls = objc_getClass(save ? "NSSavePanel" : "NSOpenPanel");
    id panel;
    CFStringRef title;
    long r;

    res->status = IT_DLG_UNAVAILABLE;
    if (!cls)
        goto out;
    panel = msg0((id)cls, save ? "savePanel" : "openPanel");
    if (!panel)
        goto out;

    title = cfstr(Screen_DialogTitle(req->kind));
    msg_id(panel, "setTitle:", (id)title);
    msg_id(panel, "setMessage:", (id)title);    /* panels may hide titles */
    CFRelease(title);

    if (save) {
        CFArrayRef types = ext_array("it;s3m");
        CFStringRef name = cfstr(req->suggest_name);
        msg_id(panel, "setAllowedFileTypes:", (id)types);
        msg_bool(panel, "setAllowsOtherFileTypes:", YES);
        msg_bool(panel, "setCanCreateDirectories:", YES);
        if (req->suggest_name && *req->suggest_name)
            msg_id(panel, "setNameFieldStringValue:", (id)name);
        CFRelease(name);
        CFRelease(types);
    } else {
        msg_bool(panel, "setCanChooseFiles:", folder ? NO : YES);
        msg_bool(panel, "setCanChooseDirectories:", folder ? YES : NO);
        msg_bool(panel, "setAllowsMultipleSelection:", NO);
        if (folder) {
            msg_bool(panel, "setCanCreateDirectories:", YES);
        } else {
            CFArrayRef types = ext_array(Screen_DialogExts(req->kind));
            msg_id(panel, "setAllowedFileTypes:", (id)types);
            CFRelease(types);
        }
    }
    set_start_dir(panel, req->start_dir);

    r = ((long (*)(id, SEL))objc_msgSend)(panel, SEL_("runModal"));
    if (r != 1) {                       /* NSModalResponseOK */
        res->status = IT_DLG_CANCELLED;
    } else {
        id url = msg0(panel, "URL");
        id path = url ? msg0(url, "path") : NULL;
        const char *u8 = path
            ? ((const char *(*)(id, SEL))objc_msgSend)(path, SEL_("UTF8String"))
            : NULL;
        if (!u8) {
            res->status = IT_DLG_CANCELLED;
        } else if (strlen(u8) >= sizeof(res->path) - 1) {
            res->status = IT_DLG_REJECTED;
            res->reason = "Path too long";
        } else {
            memcpy(res->path, u8, strlen(u8) + 1);
            res->lossy = Screen_Utf8ToCP437Display(res->path, res->display,
                                                   sizeof(res->display));
            if (save)
                res->save_format = Screen_SaveFormatFromName(res->path);
            res->status = IT_DLG_CHOSEN;
        }
    }

out:
    objc_autoreleasePoolPop(pool);
    return res->status;
}

#else
typedef int it_dialog_mac_unused;       /* ISO C: no empty translation unit */
#endif
