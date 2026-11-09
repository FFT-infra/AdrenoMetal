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
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" int nativeInstallFix(int attempt);

#define MARKER "/private/var/ngpu/lsfix.ok"
#define ATTEMPTS 24
#define INTERVAL_US (5 * 1000 * 1000)

static void say(const char *text) {
    int fd = open("/dev/console", O_WRONLY);
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
