#include "oinit.h"
#include <sys/reboot.h>
#include <linux/reboot.h>

/* inittab entry */
#define INITTAB_PATH "/etc/inittab"
#define MAX_ENTRIES  64

enum action {
    ACT_SYSINIT,
    ACT_RESPAWN,
    ACT_ONCE,
    ACT_WAIT,
    ACT_SHUTDOWN,
    ACT_RESTART,
    ACT_CTRLALTDEL,
    ACT_ASK,
};

struct entry {
    char  id[8];
    char  tty[64];
    enum  action act;
    char  cmd[256];
    pid_t pid;
};

static struct entry tab[MAX_ENTRIES];
static int n_entries;

static enum action parse_action(const char *s) {
    if (!strcmp(s, "sysinit"))    return ACT_SYSINIT;
    if (!strcmp(s, "respawn"))    return ACT_RESPAWN;
    if (!strcmp(s, "once"))       return ACT_ONCE;
    if (!strcmp(s, "wait"))       return ACT_WAIT;
    if (!strcmp(s, "shutdown"))   return ACT_SHUTDOWN;
    if (!strcmp(s, "restart"))    return ACT_RESTART;
    if (!strcmp(s, "ctrlaltdel")) return ACT_CTRLALTDEL;
    if (!strcmp(s, "askfirst"))   return ACT_ASK;
    return ACT_ONCE;
}

static void parse_inittab(void) {
    FILE *f = fopen(INITTAB_PATH, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f) && n_entries < MAX_ENTRIES) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        /* format: id:tty:action:cmd */
        char id[8]={0}, tty[64]={0}, act_s[32]={0}, cmd[256]={0};
        char *f1 = strtok(p,   ":");
        char *f2 = strtok(NULL,":");
        char *f3 = strtok(NULL,":");
        char *f4 = strtok(NULL,"\n");
        if (!f3) continue;
        if (f1) strncpy(id,    f1, sizeof(id)-1);
        if (f2) strncpy(tty,   f2, sizeof(tty)-1);
        if (f3) strncpy(act_s, f3, sizeof(act_s)-1);
        if (f4) strncpy(cmd,   f4, sizeof(cmd)-1);
        struct entry *e = &tab[n_entries++];
        strncpy(e->id,  id,  sizeof(e->id));
        strncpy(e->tty, tty, sizeof(e->tty));
        e->act = parse_action(act_s);
        strncpy(e->cmd, cmd, sizeof(e->cmd));
        e->pid = -1;
    }
    fclose(f);
}

static pid_t spawn_entry(struct entry *e) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid > 0) return pid;

    /* child: open tty if specified */
    setsid();
    if (e->tty[0]) {
        char dev[80];
        snprintf(dev, sizeof(dev), "/dev/%s", e->tty);
        int fd = open(dev, O_RDWR);
        if (fd >= 0) {
            ioctl(fd, TIOCSCTTY, 0);
            dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
            if (fd > 2) close(fd);
        }
    }

    if (e->act == ACT_ASK) {
        printf("Press Enter to activate this console\n");
        getchar();
    }

    /* exec via sh */
    execl("/bin/sh", "sh", "-c", e->cmd, NULL);
    /* try oinit itself as shell */
    execl("/init", "sh", "-c", e->cmd, NULL);
    _exit(127);
}

static void run_action_type(enum action act, int wait_for) {
    for (int i = 0; i < n_entries; i++) {
        struct entry *e = &tab[i];
        if (e->act != act) continue;
        pid_t pid = spawn_entry(e);
        if (pid > 0 && wait_for) {
            int st;
            waitpid(pid, &st, 0);
        } else if (pid > 0) {
            e->pid = pid;
        }
    }
}

static void sysrq(char cmd) {
    int fd = open("/proc/sysrq-trigger", O_WRONLY);
    if (fd >= 0) { write(fd, &cmd, 1); close(fd); }
}

/* track which dirs were created by us so we can clean them up */
static int created_dev=0, created_proc=0, created_sys=0;

static void ensure_mount(const char *path, const char *fstype, const char *src, int *created) {
    struct stat st;
    if (stat(path, &st) < 0) {
        mkdir(path, 0755);
        *created = 1;
    }
    mount(src, path, fstype, 0, NULL);
}

static volatile int got_sigchld;
static volatile int do_reboot;
static volatile int do_poweroff;
static volatile int do_restart;

static void sigchld_handler(int sig) { (void)sig; got_sigchld = 1; }
static void sigterm_handler(int sig) { (void)sig; do_poweroff = 1; }
static void sigusr1_handler(int sig) { (void)sig; do_poweroff = 1; }
static void sigusr2_handler(int sig) { (void)sig; do_reboot = 1; }
static void sigint_handler(int sig)  { (void)sig; /* ctrlaltdel */ }

static void reap_children(void) {
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        for (int i = 0; i < n_entries; i++) {
            struct entry *e = &tab[i];
            if (e->pid == pid) {
                e->pid = -1;
                break;
            }
        }
    }
}

static void respawn_needed(void) {
    for (int i = 0; i < n_entries; i++) {
        struct entry *e = &tab[i];
        if ((e->act == ACT_RESPAWN || e->act == ACT_ASK) && e->pid <= 0)
            e->pid = spawn_entry(e);
    }
}

static void do_shutdown(int rb) {
    /* kill all processes */
    kill(-1, SIGTERM);
    sleep(1);
    kill(-1, SIGKILL);
    sleep(1);
    /* run shutdown entries */
    run_action_type(ACT_SHUTDOWN, 1);
    /* sync */
    sync(); sync(); sync();
    /* unmount our created mountpoints */
    if (created_sys)  { umount2("/sys",  MNT_DETACH); rmdir("/sys");  }
    if (created_proc) { umount2("/proc", MNT_DETACH); rmdir("/proc"); }
    if (created_dev)  { umount2("/dev",  MNT_DETACH); rmdir("/dev");  }
    /* unmount all remaining */
    sysrq('u');
    sleep(1);
    if (rb) sysrq('b');
    else    sysrq('o');
    sleep(5);
    /* fallback */
    if (rb) reboot(RB_AUTOBOOT);
    else    reboot(RB_POWER_OFF);
}

int init_main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* mount essentials - create dirs if missing, track for cleanup */
    ensure_mount("/dev",  "devtmpfs", "devtmpfs", &created_dev);
    ensure_mount("/proc", "proc",     "proc",      &created_proc);
    ensure_mount("/sys",  "sysfs",    "sysfs",     &created_sys);

    /* signals */
    signal(SIGCHLD,  sigchld_handler);
    signal(SIGTERM,  sigterm_handler);
    signal(SIGUSR1,  sigusr1_handler);
    signal(SIGUSR2,  sigusr2_handler);
    signal(SIGINT,   sigint_handler);
    signal(SIGHUP,   SIG_IGN);

    parse_inittab();

    /* sysinit: run sequentially */
    run_action_type(ACT_SYSINIT, 1);

    /* wait entries */
    run_action_type(ACT_WAIT, 1);

    /* once entries */
    run_action_type(ACT_ONCE, 0);

    /* initial respawn */
    respawn_needed();

    /* main loop */
    for (;;) {
        if (do_reboot)   { do_shutdown(1); }
        if (do_poweroff) { do_shutdown(0); }
        if (do_restart)  {
            do_restart = 0;
            run_action_type(ACT_RESTART, 1);
        }
        if (got_sigchld) {
            got_sigchld = 0;
            reap_children();
            respawn_needed();
        }
        pause();
    }
    return 0;
}
