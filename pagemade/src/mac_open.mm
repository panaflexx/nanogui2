/*
 * GLFW's application delegate does not implement application:openFiles:, so
 * Launch Services starts the app and then drops the document. Add that
 * method before NSApp finishes launching (the open event is delivered
 * inside glfwInit's first [NSApp run]).
 */
#include "mac_open.h"

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#include <cstdio>
#include <mutex>

namespace {

std::mutex g_mu;
std::vector<std::string> g_pending;
std::function<void(const std::string &)> g_callback;

void open_files(id, SEL, NSApplication *sender, NSArray<NSString *> *filenames) {
    std::vector<std::string> paths;
    for (NSString *name in filenames) {
        if (name.length == 0)
            continue;
        paths.emplace_back(name.fileSystemRepresentation);
    }
    std::function<void(const std::string &)> cb;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        cb = g_callback;
        if (!cb)
            g_pending.insert(g_pending.end(), paths.begin(), paths.end());
    }
    if (cb) {
        for (const std::string &path : paths)
            cb(path);
    }
    [sender replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}

__attribute__((constructor))
void pagemade_install_mac_open() {
    Class delegate = objc_getClass("GLFWApplicationDelegate");
    if (!delegate) {
        std::fprintf(stderr, "pagemade: Finder open handler not installed\n");
        return;
    }
    class_addMethod(delegate, @selector(application:openFiles:),
                    (IMP) open_files, "v@:@@");
}

} // namespace

std::vector<std::string> take_pending_mac_opens() {
    std::lock_guard<std::mutex> lock(g_mu);
    std::vector<std::string> out;
    out.swap(g_pending);
    return out;
}

void set_mac_open_callback(const std::function<void(const std::string &)> &cb) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_callback = cb;
}
