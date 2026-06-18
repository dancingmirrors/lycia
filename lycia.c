#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

#define POLL_SECONDS 30
#define DRIVER_DIRECTORY "/sys/bus/pci/drivers/snd_hda_intel"
#define POWER_SAVE_NODE "/sys/module/snd_hda_intel/parameters/power_save"

#define VM_DIRTY_BYTES "/proc/sys/vm/dirty_bytes"
#define VM_DIRTY_BG_BYTES "/proc/sys/vm/dirty_background_bytes"
#define DIRTY_BYTES_STR "67108864"
#define DIRTY_BG_BYTES_STR "16777216"
#define BLOCK_DIRECTORY "/sys/block"

static const char *const SCHED_PREFERENCE[] = {
    "bfq", "mq-deadline", "deadline", NULL
};

static volatile sig_atomic_t g_quit = 0;

static void on_signal(int sig) {
    (void) sig;
    g_quit = 1;
}

static void daemonize(void) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }
    if (setsid() < 0) {
        perror("setsid");
        exit(EXIT_FAILURE);
    }
    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }
    umask(0);
    if (chdir("/") < 0) {
        perror("chdir");
        exit(EXIT_FAILURE);
    }

    int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null_fd < 0) {
        perror("open /dev/null");
        exit(EXIT_FAILURE);
    }

    dup2(null_fd, STDIN_FILENO);
    dup2(null_fd, STDOUT_FILENO);
    dup2(null_fd, STDERR_FILENO);
    if (null_fd > STDERR_FILENO) {
        close(null_fd);
    }
}

static int sysfs_write(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        syslog(LOG_WARNING, "open(%s): %s", path, strerror(errno));
        return -1;
    }

    size_t len = strlen(value);
    ssize_t n = write(fd, value, len);
    int err = errno;
    close(fd);

    if (n < 0) {
        syslog(LOG_WARNING, "write(%s): %s", path, strerror(err));
        return -1;
    }
    if ((size_t) n != len) {
        syslog(LOG_WARNING, "write(%s): short write (%zd/%zu)", path, n, len);
        return -1;
    }

    return 0;
}

static int sysfs_try(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    size_t len = strlen(value);
    ssize_t n = write(fd, value, len);
    close(fd);

    return ((size_t) n == len) ? 0 : -1;
}

static int looks_like_pci(const char *name) {
    return strlen(name) >= 7
        && strchr(name, ':') != NULL && strchr(name, '.') != NULL;
}

static void apply_wakelock(void) {
    sysfs_write(POWER_SAVE_NODE, "0");

    DIR *dir = opendir(DRIVER_DIRECTORY);
    if (!dir) {
        if (errno != ENOENT) {
            syslog(LOG_WARNING, "opendir(%s): %s",
                   DRIVER_DIRECTORY, strerror(errno));
        }
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!looks_like_pci(ent->d_name)) {
            continue;
        }
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s/power/control",
                 DRIVER_DIRECTORY, ent->d_name);
        sysfs_write(path, "on");
    }

   closedir(dir);
}

static int is_rotational(const char *dev) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s/queue/rotational", BLOCK_DIRECTORY,
             dev);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }

    char buf[4] = { 0 };
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    return n > 0 && buf[0] == '1';
}

static int is_skippable(const char *name) {
    if (name[0] == '.') {
        return 1;
    }
    static const char *const pfx[] = {
        "loop", "zram", "ram", "dm-", "md", "sr", NULL
    };
    for (int i = 0; pfx[i]; i++) {
        if (strncmp(name, pfx[i], strlen(pfx[i])) == 0) {
            return 1;
        }
    }

    return 0;
}

static void apply_scheduler(const char *dev) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s/queue/scheduler", BLOCK_DIRECTORY,
             dev);

    for (int i = 0; SCHED_PREFERENCE[i]; i++) {
        if (sysfs_try(path, SCHED_PREFERENCE[i]) == 0) {
            return;
        }
    }

    syslog(LOG_WARNING, "%s: no suitable I/O scheduler found!", dev);
}

static void apply_disk_tunables(void) {
    sysfs_write(VM_DIRTY_BYTES, DIRTY_BYTES_STR);
    sysfs_write(VM_DIRTY_BG_BYTES, DIRTY_BG_BYTES_STR);

    DIR *dir = opendir(BLOCK_DIRECTORY);
    if (!dir) {
        syslog(LOG_WARNING, "opendir(%s): %s", BLOCK_DIRECTORY,
               strerror(errno));
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (is_skippable(ent->d_name)) {
            continue;
        }
        if (is_rotational(ent->d_name)) {
            apply_scheduler(ent->d_name);
        }
    }

    closedir(dir);
}

int main(void) {
    if (geteuid() != 0) {
        fprintf(stderr, "Must run as root.\n");
        return 1;
    }

    daemonize();

    openlog("lycia", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, SIG_IGN);

    while (!g_quit) {
        apply_wakelock();
        apply_disk_tunables();
        sleep(POLL_SECONDS);
    }

    closelog();

    return 0;
}
