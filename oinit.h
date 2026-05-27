#ifndef OINIT_H
#define OINIT_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <signal.h>
#include <termios.h>
#include <dirent.h>
#include <glob.h>
#include <grp.h>
#include <pwd.h>
#include <time.h>
#include <limits.h>
#include <fnmatch.h>
#include <ctype.h>
#include <regex.h>
#include <sys/sysmacros.h>

/* ---- applet table ---- */
struct applet {
    const char *name;
    int (*main)(int argc, char **argv);
};
extern const struct applet applets[];
extern const int n_applets;

/* ---- entry points ---- */
int shell_main(int argc, char **argv);
int init_main(int argc, char **argv);

/* ---- shell built-ins exposed for applets ---- */
int builtin_printf_fn(int argc, char **argv);
int builtin_test(int argc, char **argv);

/* ---- applets (applets.c) ---- */
int applet_cat(int, char **);
int applet_ls(int, char **);
int applet_cp(int, char **);
int applet_mv(int, char **);
int applet_rm(int, char **);
int applet_mkdir(int, char **);
int applet_rmdir(int, char **);
int applet_ln(int, char **);
int applet_chmod(int, char **);
int applet_chown(int, char **);
int applet_touch(int, char **);
int applet_stat(int, char **);
int applet_echo(int, char **);
int applet_printf(int, char **);
int applet_test(int, char **);
int applet_true(int, char **);
int applet_false(int, char **);
int applet_grep(int, char **);
int applet_sed(int, char **);
int applet_cut(int, char **);
int applet_tr(int, char **);
int applet_wc(int, char **);
int applet_head(int, char **);
int applet_tail(int, char **);
int applet_sort(int, char **);
int applet_uniq(int, char **);
int applet_tee(int, char **);
int applet_find(int, char **);
int applet_xargs(int, char **);
int applet_mount(int, char **);
int applet_umount(int, char **);
int applet_sync(int, char **);
int applet_ps(int, char **);
int applet_kill(int, char **);
int applet_env(int, char **);
int applet_uname(int, char **);
int applet_hostname(int, char **);
int applet_pwd(int, char **);
int applet_date(int, char **);
int applet_sleep(int, char **);
int applet_seq(int, char **);
int applet_yes(int, char **);
int applet_id(int, char **);
int applet_whoami(int, char **);
int applet_basename(int, char **);
int applet_dirname(int, char **);
int applet_readlink(int, char **);
int applet_realpath(int, char **);
int applet_which(int, char **);
int applet_dd(int, char **);
int applet_hexdump(int, char **);
int applet_reboot(int, char **);
int applet_shutdown(int, char **);
int applet_poweroff(int, char **);
int applet_dmesg(int, char **);
int applet_free(int, char **);
int applet_df(int, char **);
int applet_du(int, char **);
int applet_mkfifo(int, char **);
int applet_mknod(int, char **);
int applet_tty(int, char **);
int applet_stty(int, char **);
int applet_nohup(int, char **);
int applet_nice(int, char **);

/* ---- utils ---- */
void  die(const char *fmt, ...);
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t s);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...);
int   xopen(const char *path, int flags, mode_t mode);

#define ARRLEN(a) ((int)(sizeof(a)/sizeof(*(a))))

#endif
