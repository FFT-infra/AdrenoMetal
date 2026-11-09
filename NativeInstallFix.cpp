// SPDX-License-Identifier: MIT
// Owner LaunchServices repair. launchd starts it from the owner job over /System/Library/LaunchDaemons (the boot-time addon mount), so
// it runs as root outside every sandbox profile. The image never runs MobileInstallation, so the LaunchServices database keeps the
// container paths of the machine it was migrated from and PlugInKit refuses every /Applications plugin ("plug-ins outside containing
// apps must be in /System"). This daemon performs that registration once per Data volume. The first route is the call the system's own
// DataClassMigrator makes (`-[LSApplicationWorkspace _LSPrivateRebuildApplicationDatabasesForSystemApps:internal:user:]`, which is what
// `mobile_install rebuild system user` does); the fallback registers every application URL with `LSRegisterURL`. The result goes to
// /private/var/ngpu/lsfix.log, /dev/console and the kernel probe that dumps the log (BootDiagExport.c, lsfix=<seconds>).
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/task_special_ports.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define LOG_PATH "/private/var/ngpu/lsfix.log"
#define DONE_PATH "/private/var/ngpu/lsfix.ok"

static int gLog = -1, gConsole = -1;

static void logf(const char *fmt, ...) {
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    line[n++] = '\n';
    if (gLog >= 0) (void)!write(gLog, line, (size_t)n);
    if (gConsole >= 0) (void)!write(gConsole, line, (size_t)n);
}

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// The repair must reach lsd through launchd's XPC bootstrap. This process is a child of the init
// trampoline, so it was forked before launchd ran and has no bootstrap port of its own; launchd's
// task holds it. Root with the task_for_pid grant can copy it. Without this the LaunchServices call
// has nowhere to go and returns NO (in2, 2026-11-09: the selector answered, the rebuild returned 0).
// Declared here: the iOS SDK ships no servers/bootstrap.h, and the symbol is in libSystem.
extern "C" kern_return_t bootstrap_look_up(mach_port_t bp, const char *service_name, mach_port_t *service_port);

static int gBootstrapAdopted;
// 1 when this process can reach the system bootstrap. Two sources, in order: the kernel hands this
// task a port copied from a process launchd already started (BootDiagExport.c ls_bootstrap_gift,
// the only source that works here: task_for_pid reaches pid 1 only and launchd's own task carries
// no bootstrap, in5/in6), then the plain scan for the case the kernel gift is absent.
static int usableBootstrap(mach_port_t bootstrap, int *looked) {
    mach_port_t service = MACH_PORT_NULL;
    const kern_return_t kr = bootstrap_look_up(bootstrap, "com.apple.lsd.rebuild", &service);
    if (service != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), service);
    if (looked) *looked = kr;
    return kr == KERN_SUCCESS;
}

static int adoptLaunchdBootstrap(void) {
    if (gBootstrapAdopted) return 1;
    mach_port_t own = MACH_PORT_NULL;
    if (task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &own) == KERN_SUCCESS && own != MACH_PORT_NULL) {
        int looked = -1;
        const int usable = usableBootstrap(own, &looked);
        if (!usable) mach_port_deallocate(mach_task_self(), own);
        logf("task bootstrap present lsd=%d", looked);
        if (usable) {
            gBootstrapAdopted = 1;
            return 1;
        }
    }
    int tasks = 0, bootstraps = 0, known = 0, shown = 0;
    for (int pid = 1; pid <= 400; ++pid) {
        mach_port_t task = MACH_PORT_NULL;
        if (task_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS) continue;
        ++tasks;
        mach_port_t bootstrap = MACH_PORT_NULL;
        const kern_return_t got = task_get_special_port(task, TASK_BOOTSTRAP_PORT, &bootstrap);
        mach_port_deallocate(mach_task_self(), task);
        if (got != KERN_SUCCESS || bootstrap == MACH_PORT_NULL) {
            if (shown < 8) { logf("scan pid=%d task=1 bootstrap=0", pid); ++shown; }
            continue;
        }
        ++bootstraps;
        int looked = -1;
        const int usable = usableBootstrap(bootstrap, &looked);
        if (shown < 8) { logf("scan pid=%d task=1 bootstrap=1 lsd=%d", pid, looked); ++shown; }
        if (!usable) {
            mach_port_deallocate(mach_task_self(), bootstrap);
            continue;
        }
        ++known;
        const kern_return_t set = task_set_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, bootstrap);
        logf("adopted bootstrap from pid %d set=%d (tasks=%d bootstraps=%d)", pid, set, tasks, bootstraps);
        if (set != KERN_SUCCESS) return 0;
        gBootstrapAdopted = 1;
        return 1;
    }
    logf("no system bootstrap: tasks=%d bootstraps=%d known=%d", tasks, bootstraps, known);
    return 0;
}

// The class and selector the system's 00LaunchServicesMigrator uses for the same job.
static int rebuildClassDatabases(void) {
    void *core = dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW | RTLD_GLOBAL);
    logf("dlopen CoreServices: %d", core != nullptr);
    Class workspaceClass = objc_getClass("LSApplicationWorkspace");
    logf("LSApplicationWorkspace class: %d", workspaceClass != Nil);
    if (!workspaceClass) return -1;
    id workspace = ((id (*)(id, SEL))(void *)objc_msgSend)((id)workspaceClass, sel_registerName("defaultWorkspace"));
    logf("defaultWorkspace: %d", workspace != nil);
    if (!workspace) return -2;
    SEL rebuild = sel_registerName("_LSPrivateRebuildApplicationDatabasesForSystemApps:internal:user:");
    const int responds = ((BOOL (*)(id, SEL, SEL))(void *)objc_msgSend)(workspace, sel_registerName("respondsToSelector:"), rebuild);
    logf("responds to rebuild selector: %d", responds);
    if (!responds) return -3;
    // BOOL, BOOL, BOOL arguments: system apps yes, internal no, user yes.
    using RebuildFn = BOOL (*)(id, SEL, BOOL, BOOL, BOOL);
    RebuildFn call = (RebuildFn)((void *(*)(id, SEL, SEL))(void *)objc_msgSend)(workspace, sel_registerName("methodForSelector:"), rebuild);
    if (!call) return -4;
    const BOOL result = call(workspace, rebuild, YES, NO, YES);
    logf("rebuild application databases returned %d", result);
    return result ? 0 : -5;
}

// Fallback: register every application bundle one at a time.
static int registerWithLaunchServices(int *ok, int *failed) {
    void *cs = dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW | RTLD_LOCAL);
    logf("dlopen CoreServices for LSRegisterURL: %d", cs != nullptr);
    if (!cs) return -1;
    using RegisterFn = int (*)(CFURLRef, CFURLRef *);
    RegisterFn reg = (RegisterFn)dlsym(cs, "LSRegisterURL");
    logf("dlsym LSRegisterURL: %d", reg != nullptr);
    if (!reg) reg = (RegisterFn)dlsym(cs, "_LSRegisterURL");
    logf("dlsym _LSRegisterURL: %d", reg != nullptr);
    if (!reg) return -2;
    DIR *dir = opendir("/Applications");
    if (!dir) {
        logf("opendir /Applications failed: %d", errno);
        return -3;
    }
    *ok = 0;
    *failed = 0;
    unsigned seen = 0;
    while (struct dirent *entry = readdir(dir)) {
        const char *name = entry->d_name;
        const size_t n = strlen(name);
        if (n < 5 || strcmp(name + n - 4, ".app") != 0) continue;
        ++seen;
        char path[512];
        snprintf(path, sizeof path, "/Applications/%s", name);
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)path, (CFIndex)strlen(path), true);
        if (!url) {
            ++*failed;
            continue;
        }
        CFURLRef error = nullptr;
        const int status = reg(url, &error);
        CFRelease(url);
        if (error) {
            if (*failed < 6) {
                CFStringRef text = CFURLGetString(error);
                char shown[200] = "";
                if (text) CFStringGetCString(text, shown, sizeof shown, kCFStringEncodingUTF8);
                logf("reg %s status=%d %s", name, status, shown);
                CFRelease(error);
            }
            ++*failed;
        } else {
            ++*ok;
        }
    }
    closedir(dir);
    logf("apps seen=%u registered=%d failed=%d", seen, *ok, *failed);
    return 0;
}

// One attempt. Returns 0 when the repair ran or was already done, nonzero when it has to be
// retried: before launchd runs there is no XPC bootstrap at all, so the LaunchServices call fails
// and the caller's retry carries it past launchd's start. attempt numbers the rows.
extern "C" int nativeInstallFix(int attempt) {
    char bootargs[2048];
    size_t length = sizeof bootargs;
    if (sysctlbyname("kern.bootargs", bootargs, &length, nullptr, 0) == 0 && strstr(bootargs, "sheng-lsfix=0")) return 0;
    if (exists(DONE_PATH)) return 0;
    gLog = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    gConsole = open("/dev/console", O_WRONLY);
    logf("lsfix attempt=%d start uid=%d", attempt, getuid());
    logf("paths mobile_install=%d installd=%d mi_framework=%d", exists("/usr/bin/mobile_install"), exists("/usr/libexec/installd"),
         exists("/System/Library/PrivateFrameworks/MobileInstallation.framework"));
    int done = 0;
    if (!adoptLaunchdBootstrap()) {
        logf("lsfix attempt=%d no system bootstrap yet", attempt);
        if (gConsole >= 0) close(gConsole);
        if (gLog >= 0) close(gLog);
        gConsole = gLog = -1;
        return 1;
    }
    const int rebuilt = rebuildClassDatabases();
    logf("rebuild rc=%d", rebuilt);
    if (rebuilt == 0) done = 1;
    if (!done) {
        int ok = 0, failed = 0;
        const int status = registerWithLaunchServices(&ok, &failed);
        logf("registration rc=%d ok=%d failed=%d", status, ok, failed);
        if (status == 0 && ok > 0) done = 1;
    }
    if (done) {
        int fd = open(DONE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
    }
    logf("lsfix attempt=%d done=%d", attempt, done);
    if (gConsole >= 0) close(gConsole);
    if (gLog >= 0) close(gLog);
    gConsole = gLog = -1;
    return done ? 0 : 1;
}
