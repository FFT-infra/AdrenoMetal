// SPDX-License-Identifier: MIT
// Owner init trampoline: the kernel runs this as process 1 when boot-arg sheng-init=1 and
// /private/var/ngpu/lsfix.ok is absent (patches/xnu/native-init-trampoline.patch). launchd's system
// jobs come from the signed cache in /System/Library/xpc/launchd.plist, so the owner job the GPU addon
// mounts over /System/Library/LaunchDaemons is never loaded; this is the channel that does not need it.
// The repair needs launchd's XPC bootstrap, which does not exist yet while this runs (lx1/in2 measured
// the rebuild returning NO at that point), so it runs in a detached child that retries past launchd's
// start while process 1 execs /sbin/launchd at once.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" int nativeInstallFix(int attempt);

#define MARKER "/private/var/ngpu/lsfix.ok"
#define ATTEMPTS 24
#define INTERVAL_US (5 * 1000 * 1000)

// /dev/console does not reach the USB console here; the kernel probe reads this file instead.
static void say(const char *text) {
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        (void)!write(fd, text, strlen(text));
        close(fd);
    }
    fd = open("/private/var/ngpu/lsinit.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        (void)!write(fd, text, strlen(text));
        close(fd);
    }
}

// Runs after the parent execs launchd. Every attempt opens its own log line, so a boot that was read
// at t=120 shows how many attempts it took and what each one returned.
static void repair(void) {
    (void)setsid();
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, 0);
        dup2(devnull, 1);
        dup2(devnull, 2);
        if (devnull > 2) close(devnull);
    }
    for (int attempt = 1; attempt <= ATTEMPTS; ++attempt) {
        if (nativeInstallFix(attempt) == 0) {
            say("LSINIT repair done\n");
            return;
        }
        usleep(INTERVAL_US);
    }
    say("LSINIT repair gave up\n");
}

// launchd starts /usr/libexec/pkd for PlugInKit discovery, and only launchd's own children have the
// XPC bootstrap the LaunchServices call needs. The trampoline therefore copies the real daemon out of
// the way first, then mounts the addon's directory over /usr/libexec as a union, so the name pkd
// answers with the owner shim (NativePkdShim.cpp) while every other name stays the sealed file. A
// refused copy or mount leaves the system exactly as it was.
static int __attribute__((unused)) copyFile(const char *from, const char *to) {
    const int in = open(from, O_RDONLY);
    if (in < 0) return -1;
    const int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0) { close(in); return -2; }
    char buffer[65536];
    ssize_t got;
    int result = 0;
    while ((got = read(in, buffer, sizeof buffer)) > 0) {
        ssize_t sent = 0;
        while (sent < got) {
            const ssize_t n = write(out, buffer + sent, (size_t)(got - sent));
            if (n <= 0) { result = -3; break; }
            sent += n;
        }
        if (result) break;
    }
    if (got < 0 && !result) result = -4;
    close(in);
    close(out);
    return result;
}

static void installCacheView(void) {
    char text[128];
    if (mount("bindfs", "/System/Library/xpc", MNT_RDONLY | MNT_UNION, (void *)"/private/var/ngpu/xpc") != 0) {
        snprintf(text, sizeof text, "LSINIT xpc view mount failed %d\n", errno);
        say(text);
        return;
    }
    say("LSINIT xpc view mounted\n");
}

// The owner service under a system path. launchd's default system profile refused to posix_spawn
// /private/var/ngpu/bin/adrenoprobed with EPERM; the documented owner-service exposure is a
// read-only union mount at a system path (docs/zh/drivers/ath12k-wifi.md, /usr/libexec/
// unixnu-wpa_supplicant on sheng), so the job's program is /usr/libexec/adrenoprobed and this view
// answers that name. The lower layer stays the sealed /usr/libexec for every other name.
static void __attribute__((unused)) installLibexecView(void) {
    char text[128];
    if (mount("bindfs", "/usr/libexec", MNT_RDONLY | MNT_UNION, (void *)"/private/var/ngpu/usrlibexec") != 0) {
        snprintf(text, sizeof text, "LSINIT libexec view mount failed %d\n", errno);
        say(text);
        return;
    }
    say("LSINIT libexec view mounted\n");
}

// The DataClassMigrator route: the system's own 00LaunchServicesMigrator reads
// _LSPrivateDatabaseNeedsRebuild from the standard defaults of the process that runs migrators and,
// when it is set, calls the same LaunchServices rebuild this trampoline could not reach (no XPC
// bootstrap before launchd). The migrator is started by launchd, so it has one. Written before
// launchd runs, in both homes the migrator can have.
static const char migration_request[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\"><dict><key>_LSPrivateDatabaseNeedsRebuild</key><true/></dict></plist>\n";

static void requestMigration(const char *home) {
    char dir[160], path[200];
    snprintf(dir, sizeof dir, "%s/Library/Preferences", home);
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/Library", home);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/Library/Preferences/com.apple.datamigrator.plist", home);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)!write(fd, migration_request, strlen(migration_request));
        close(fd);
    }
}

int main() {
    struct stat st;
    if (stat(MARKER, &st) != 0) {
        say("LSINIT trampoline enter\n");
        pid_t child = fork();
        if (child == 0) {
            repair();
            _exit(0);
        }
    }
    {
        char args[1024];
        size_t size = sizeof args;
        if (sysctlbyname("kern.bootargs", args, &size, nullptr, 0) == 0 && strstr(args, "sheng-xpccache=1")) {
            installCacheView();
            // installLibexecView() is not called: in17-in20, the only boots that mount a union over
            // /usr/libexec, never reached a desktop while in15/in16 (xpc view only) did. The launchd
            // route it served is closed anyway (launch constraints); the mount is the prime suspect
            // for the regression and is measured off in in21.
        }
    }
    requestMigration("/private/var/mobile");
    requestMigration("/private/var/root");
    char *argv[] = {(char *)"/sbin/launchd", nullptr};
    execv("/sbin/launchd", argv);
    const int error = errno;
    char text[96];
    snprintf(text, sizeof text, "LSINIT launchd exec failed %d\n", error);
    say(text);
    return 127;
}
