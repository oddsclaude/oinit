#include "oinit.h"

const struct applet applets[] = {
    {"[",        applet_test},
    {"basename", applet_basename},
    {"cat",      applet_cat},
    {"chmod",    applet_chmod},
    {"chown",    applet_chown},
    {"cp",       applet_cp},
    {"cut",      applet_cut},
    {"date",     applet_date},
    {"dd",       applet_dd},
    {"df",       applet_df},
    {"dirname",  applet_dirname},
    {"dmesg",    applet_dmesg},
    {"du",       applet_du},
    {"echo",     applet_echo},
    {"env",      applet_env},
    {"false",    applet_false},
    {"find",     applet_find},
    {"free",     applet_free},
    {"grep",     applet_grep},
    {"hexdump",  applet_hexdump},
    {"hostname", applet_hostname},
    {"id",       applet_id},
    {"kill",     applet_kill},
    {"ln",       applet_ln},
    {"ls",       applet_ls},
    {"mkdir",    applet_mkdir},
    {"mkfifo",   applet_mkfifo},
    {"mknod",    applet_mknod},
    {"mount",    applet_mount},
    {"mv",       applet_mv},
    {"nice",     applet_nice},
    {"nohup",    applet_nohup},
    {"od",       applet_hexdump},
    {"poweroff", applet_poweroff},
    {"printf",   applet_printf},
    {"ps",       applet_ps},
    {"pwd",      applet_pwd},
    {"readlink", applet_readlink},
    {"realpath", applet_realpath},
    {"reboot",   applet_reboot},
    {"rm",       applet_rm},
    {"rmdir",    applet_rmdir},
    {"sed",      applet_sed},
    {"seq",      applet_seq},
    {"shutdown", applet_shutdown},
    {"sleep",    applet_sleep},
    {"sort",     applet_sort},
    {"stat",     applet_stat},
    {"stty",     applet_stty},
    {"sync",     applet_sync},
    {"tail",     applet_tail},
    {"tee",      applet_tee},
    {"test",     applet_test},
    {"touch",    applet_touch},
    {"tr",       applet_tr},
    {"true",     applet_true},
    {"tty",      applet_tty},
    {"umount",   applet_umount},
    {"uname",    applet_uname},
    {"uniq",     applet_uniq},
    {"wc",       applet_wc},
    {"which",    applet_which},
    {"whoami",   applet_whoami},
    {"xargs",    applet_xargs},
    {"yes",      applet_yes},
};
const int n_applets = ARRLEN(applets);

static int applet_cmp(const void *a, const void *b) {
    return strcmp((const char *)a, ((const struct applet *)b)->name);
}

int run_applet(const char *name, int argc, char **argv) {
    const struct applet *a = bsearch(name, applets, n_applets,
                                     sizeof(applets[0]), applet_cmp);
    if (a) return a->main(argc, argv);
    return -1;
}

int main(int argc, char **argv) {
    const char *arg0 = argv[0];
    char *chroot_path = NULL;
    int i;

    /* strip leading path from argv[0] */
    const char *base = strrchr(arg0, '/');
    if (base) base++; else base = arg0;

    /* handle --chroot as first argument */
    if (argc >= 3 && strcmp(argv[1], "--chroot") == 0) {
        chroot_path = argv[2];
        /* shift argv */
        for (i = 1; i < argc - 2; i++) argv[i] = argv[i + 2];
        argc -= 2;
        argv[argc] = NULL;
    }

    if (chroot_path) {
        if (chroot(chroot_path) < 0) { perror("chroot"); return 1; }
        if (chdir("/") < 0) { perror("chdir"); return 1; }
    }

    /* --init: create skeleton chroot structure */
    if (argc >= 2 && strcmp(argv[1], "--init") == 0) {
        const char *root = (argc >= 3) ? argv[2] : ".";
        const char *dirs[] = {
            "bin", "sbin", "etc", "dev", "proc", "sys",
            "tmp", "var", "var/log", "var/run", "root",
            "usr", "usr/bin", "usr/sbin", "lib", "lib64", NULL
        };
        char path[4096];
        for (int d = 0; dirs[d]; d++) {
            snprintf(path, sizeof(path), "%s/%s", root, dirs[d]);
            mkdir(path, 0755);
        }
        /* /tmp: sticky+writable */
        snprintf(path, sizeof(path), "%s/tmp", root); chmod(path, 01777);
        printf("oinit: skeleton created at %s\n", root);
        printf("  install oinit to %s/bin/oinit then symlink /bin/sh, /init, applets\n", root);
        return 0;
    }

    /* PID 1 -> init */
    if (getpid() == 1)
        return init_main(argc, argv);

    /* argv[0] matches a shell name -> shell */
    if (strcmp(base, "sh")   == 0 ||
        strcmp(base, "-sh")  == 0 ||
        strcmp(base, "osh")  == 0 ||
        strcmp(base, "bash") == 0 ||
        strcmp(base, "ash")  == 0)
        return shell_main(argc, argv);

    /* argv[0] matches an applet */
    {
        int r = run_applet(base, argc, argv);
        if (r >= 0) return r;
    }

    /* explicit --shell flag */
    if (argc >= 2 && strcmp(argv[1], "--shell") == 0) {
        argv[1] = argv[0]; argv++;  argc--;
        return shell_main(argc, argv);
    }

    /* fall back: try to find applet from first real argument */
    if (argc >= 2) {
        int r = run_applet(argv[1], argc - 1, argv + 1);
        if (r >= 0) return r;
    }

    fprintf(stderr, "oinit: unknown applet '%s'\n", base);
    fprintf(stderr, "usage: oinit --shell | <applet> [args...]\n");
    return 1;
}
