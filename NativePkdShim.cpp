// SPDX-License-Identifier: MIT
// Owner pkd shim: launchd starts this binary as PlugInKitDaemon, because the init trampoline mounts
// the addon's view over /usr/libexec and that view answers the name pkd with this file. Being launchd's
// own child is the point: it has the XPC bootstrap and the mobile identity, which a process forked
// before launchd can never have (in8: every bootstrap_look_up is MACH_SEND_INVALID_DEST). The shim
// starts the LaunchServices repair in a detached child and then becomes the real daemon, which the
// trampoline copied to /private/var/ngpu/bin/pkd-real before the view was mounted.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern "C" int nativeInstallFix(int attempt);

#define STARTED "/private/var/ngpu/pkd-shim.started"
#define REAL "/private/var/ngpu/bin/pkd-real"
#define ATTEMPTS 24
#define INTERVAL_US (5 * 1000 * 1000)

static void say(const char *text) {
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        (void)!write(fd, text, strlen(text));
        close(fd);
    }
}

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
            say("PKDSHIM repair done\n");
            return;
        }
        usleep(INTERVAL_US);
    }
    say("PKDSHIM repair gave up\n");
}

int main(int argc __attribute__((unused)), char **argv) {
    say("PKDSHIM enter\n");
    const int fd = open(STARTED, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd >= 0) {
        close(fd);
        if (fork() == 0) {
            repair();
            _exit(0);
        }
    }
    execv(REAL, argv);
    char text[128];
    snprintf(text, sizeof text, "PKDSHIM real exec failed %d\n", errno);
    say(text);
    return 127;
}
