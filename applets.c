#include "oinit.h"
#include <utime.h>
extern char **environ;
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/klog.h>
#include <sys/reboot.h>
#include <linux/reboot.h>

/* ---- helpers ---- */
static void sysrq(char cmd) {
    int fd = open("/proc/sysrq-trigger", O_WRONLY);
    if (fd >= 0) { write(fd, &cmd, 1); close(fd); }
}

static mode_t parse_mode(const char *s, mode_t cur) {
    if (isdigit((unsigned char)*s)) return (mode_t)strtol(s, NULL, 8);
    mode_t m = cur;
    while (*s) {
        int who = 0;
        while (*s == 'u'||*s=='g'||*s=='o'||*s=='a') {
            if (*s=='u') who|=0100; if(*s=='g') who|=0010;
            if (*s=='o') who|=0001; if(*s=='a') who|=0111; s++;
        }
        if (!who) who = 0111;
        char op = *s ? *s++ : '=';
        mode_t bits = 0;
        while (*s=='r'||*s=='w'||*s=='x'||*s=='s'||*s=='t') {
            if (*s=='r') bits|=0444; if(*s=='w') bits|=0222;
            if (*s=='x') bits|=0111; s++;
        }
        mode_t wmask = 0;
        if (who&0100) wmask |= 0700;
        if (who&0010) wmask |= 0070;
        if (who&0001) wmask |= 0007;
        if (op=='+') m |= (bits & wmask);
        else if (op=='-') m &= ~(bits & wmask);
        else { m &= ~wmask; m |= (bits & wmask); }
        if (*s == ',') s++;
    }
    return m;
}

static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) { perror(src); return 1; }
    struct stat st; fstat(in, &st);
    int out = open(dst, O_WRONLY|O_CREAT|O_TRUNC, st.st_mode);
    if (out < 0) { perror(dst); close(in); return 1; }
    char buf[65536]; ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) write(out, buf, n);
    close(in); close(out); return 0;
}

static void print_size(long long sz) {
    const char *units[] = {"B","K","M","G","T"};
    int u = 0; double s = sz;
    while (s >= 1024 && u < 4) { s /= 1024; u++; }
    if (u) printf("%.1f%s", s, units[u]);
    else   printf("%lld%s", sz, units[0]);
}

/* ---- cat ---- */
int applet_cat(int argc, char **argv) {
    int rc = 0;
    if (argc == 1) {
        char buf[4096]; ssize_t n;
        while ((n = read(0, buf, sizeof(buf))) > 0) write(1, buf, n);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = (strcmp(argv[i],"-")==0) ? 0 : open(argv[i], O_RDONLY);
        if (fd < 0) { perror(argv[i]); rc=1; continue; }
        char buf[4096]; ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) write(1, buf, n);
        if (fd != 0) close(fd);
    }
    return rc;
}

/* ---- echo ---- */
int applet_echo(int argc, char **argv) {
    int no_nl = 0, i = 1;
    if (i < argc && strcmp(argv[i],"-n")==0) { no_nl=1; i++; }
    for (; i < argc; i++) { if(i>1+(no_nl?1:0)-1) putchar(' '); fputs(argv[i],stdout); }
    return 0;
}

/* ---- ls ---- */
int applet_ls(int argc, char **argv) {
    int show_all=0, long_fmt=0, human=0, one_col=0;
    const char *dir = ".";
    for (int i=1;i<argc;i++) {
        if (argv[i][0]=='-') {
            for (char *p=argv[i]+1;*p;p++) {
                if(*p=='a') show_all=1;
                else if(*p=='l') long_fmt=1;
                else if(*p=='h') human=1;
                else if(*p=='1') one_col=1;
            }
        } else dir=argv[i];
    }
    DIR *d = opendir(dir);
    if (!d) { perror(dir); return 1; }
    struct dirent *e;
    while ((e=readdir(d))) {
        if (!show_all && e->d_name[0]=='.') continue;
        if (long_fmt) {
            char path[PATH_MAX]; snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);
            struct stat st; lstat(path,&st);
            char perm[11]; perm[0]='?';
            if(S_ISREG(st.st_mode)) perm[0]='-';
            else if(S_ISDIR(st.st_mode)) perm[0]='d';
            else if(S_ISLNK(st.st_mode)) perm[0]='l';
            else if(S_ISCHR(st.st_mode)) perm[0]='c';
            else if(S_ISBLK(st.st_mode)) perm[0]='b';
            else if(S_ISFIFO(st.st_mode)) perm[0]='p';
            else if(S_ISSOCK(st.st_mode)) perm[0]='s';
            perm[1]=(st.st_mode&S_IRUSR)?'r':'-'; perm[2]=(st.st_mode&S_IWUSR)?'w':'-'; perm[3]=(st.st_mode&S_IXUSR)?'x':'-';
            perm[4]=(st.st_mode&S_IRGRP)?'r':'-'; perm[5]=(st.st_mode&S_IWGRP)?'w':'-'; perm[6]=(st.st_mode&S_IXGRP)?'x':'-';
            perm[7]=(st.st_mode&S_IROTH)?'r':'-'; perm[8]=(st.st_mode&S_IWOTH)?'w':'-'; perm[9]=(st.st_mode&S_IXOTH)?'x':'-';
            perm[10]='\0';
            char tmbuf[20]; struct tm *tm=localtime(&st.st_mtime); strftime(tmbuf,sizeof(tmbuf),"%b %e %H:%M",tm);
            struct passwd *pw=getpwuid(st.st_uid); struct group *gr=getgrgid(st.st_gid);
            printf("%s %3lu %-8s %-8s ", perm, (unsigned long)st.st_nlink, pw?pw->pw_name:"?", gr?gr->gr_name:"?");
            if (human) { print_size(st.st_size); putchar(' '); }
            else printf("%8lld ", (long long)st.st_size);
            printf("%s %s\n", tmbuf, e->d_name);
        } else {
            printf("%s%c", e->d_name, one_col?'\n':' ');
        }
    }
    if (!long_fmt && !one_col) putchar('\n');
    closedir(d); return 0;
}

/* ---- cp ---- */
int applet_cp(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr,"cp: missing operands\n"); return 1; }
    int rc=0, recursive=0;
    int i;
    for (i=1;i<argc&&argv[i][0]=='-';i++) {
        for (char *p=argv[i]+1;*p;p++) if(*p=='r'||*p=='R') recursive=1;
    }
    const char *dst = argv[argc-1];
    struct stat dstat; int dst_is_dir = (stat(dst,&dstat)==0 && S_ISDIR(dstat.st_mode));
    for (; i < argc-1; i++) {
        char target[PATH_MAX];
        if (dst_is_dir) snprintf(target,sizeof(target),"%s/%s",dst, strrchr(argv[i],'/')?strrchr(argv[i],'/')+1:argv[i]);
        else strncpy(target,dst,sizeof(target)-1);
        struct stat ss; lstat(argv[i],&ss);
        if (S_ISDIR(ss.st_mode) && recursive) {
            mkdir(target, ss.st_mode);
        } else {
            rc |= copy_file(argv[i], target);
        }
    }
    return rc;
}

/* ---- mv ---- */
int applet_mv(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr,"mv: missing operands\n"); return 1; }
    const char *dst = argv[argc-1];
    struct stat dstat; int dst_is_dir = (stat(dst,&dstat)==0 && S_ISDIR(dstat.st_mode));
    int rc=0;
    for (int i=1;i<argc-1;i++) {
        char target[PATH_MAX];
        if (dst_is_dir) snprintf(target,sizeof(target),"%s/%s",dst,strrchr(argv[i],'/')?strrchr(argv[i],'/')+1:argv[i]);
        else strncpy(target,dst,sizeof(target)-1);
        if (rename(argv[i],target)<0) {
            if (errno==EXDEV) { rc|=copy_file(argv[i],target); if(!rc) unlink(argv[i]); }
            else { perror(argv[i]); rc=1; }
        }
    }
    return rc;
}

/* ---- rm ---- */
int applet_rm(int argc, char **argv) {
    int rc=0, force=0, recursive=0;
    int i;
    for (i=1;i<argc&&argv[i][0]=='-';i++) {
        for (char *p=argv[i]+1;*p;p++) { if(*p=='f') force=1; if(*p=='r'||*p=='R') recursive=1; }
    }
    for (;i<argc;i++) {
        if (unlink(argv[i])<0) {
            if (recursive) { rmdir(argv[i]); }
            else if (!force) { perror(argv[i]); rc=1; }
        }
    }
    return rc;
}

/* ---- mkdir ---- */
int applet_mkdir(int argc, char **argv) {
    int rc=0, parents=0; mode_t mode=0777;
    int i;
    for (i=1;i<argc&&argv[i][0]=='-';i++) {
        for (char *p=argv[i]+1;*p;p++) {
            if(*p=='p') parents=1;
            if(*p=='m'&&argv[i+1]) { mode=parse_mode(argv[++i],0777); break; }
        }
    }
    for (;i<argc;i++) {
        if (parents) {
            char tmp[PATH_MAX]; strncpy(tmp,argv[i],sizeof(tmp)-1);
            for (char *p=tmp+1;*p;p++) if(*p=='/'){*p='\0';mkdir(tmp,mode);*p='/';}
        }
        if (mkdir(argv[i],mode)<0 && !(parents&&errno==EEXIST)) { perror(argv[i]); rc=1; }
    }
    return rc;
}

/* ---- rmdir ---- */
int applet_rmdir(int argc, char **argv) {
    int rc=0;
    for (int i=1;i<argc;i++) if(rmdir(argv[i])<0){perror(argv[i]);rc=1;}
    return rc;
}

/* ---- ln ---- */
int applet_ln(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr,"ln: missing operands\n"); return 1; }
    int sym=0,force=0;
    int i;
    for (i=1;i<argc&&argv[i][0]=='-';i++) {
        for (char *p=argv[i]+1;*p;p++) { if(*p=='s')sym=1; if(*p=='f')force=1; }
    }
    const char *dst=argv[argc-1];
    struct stat dstat; int dst_is_dir=(stat(dst,&dstat)==0&&S_ISDIR(dstat.st_mode));
    int rc=0;
    for (;i<argc-1;i++) {
        char target[PATH_MAX];
        if (dst_is_dir) snprintf(target,sizeof(target),"%s/%s",dst,strrchr(argv[i],'/')?strrchr(argv[i],'/')+1:argv[i]);
        else strncpy(target,dst,sizeof(target)-1);
        if (force) unlink(target);
        int r = sym ? symlink(argv[i],target) : link(argv[i],target);
        if (r<0) { perror(target); rc=1; }
    }
    return rc;
}

/* ---- chmod ---- */
int applet_chmod(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr,"chmod: missing operands\n"); return 1; }
    int i=1; int recursive=0;
    if (strcmp(argv[i],"-R")==0) { recursive=1; i++; }
    const char *modestr=argv[i++];
    int rc=0;
    for (;i<argc;i++) {
        struct stat st; if(lstat(argv[i],&st)<0){perror(argv[i]);rc=1;continue;}
        mode_t m=parse_mode(modestr,st.st_mode);
        if(chmod(argv[i],m)<0){perror(argv[i]);rc=1;}
    }
    return rc;
}

/* ---- chown ---- */
int applet_chown(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr,"chown: missing operands\n"); return 1; }
    int i=1;
    if (strcmp(argv[i],"-R")==0) i++;
    char *owner=argv[i++]; char *colon=strchr(owner,':');
    uid_t uid=-1; gid_t gid=-1;
    if (colon) {
        *colon='\0';
        if(*owner) { struct passwd *pw=getpwnam(owner); if(pw) uid=pw->pw_uid; else uid=atoi(owner); }
        if(colon[1]) { struct group *gr=getgrnam(colon+1); if(gr) gid=gr->gr_gid; else gid=atoi(colon+1); }
    } else { struct passwd *pw=getpwnam(owner); if(pw){uid=pw->pw_uid;gid=pw->pw_gid;}else uid=atoi(owner); }
    int rc=0;
    for (;i<argc;i++) if(lchown(argv[i],uid,gid)<0){perror(argv[i]);rc=1;}
    return rc;
}

/* ---- touch ---- */
int applet_touch(int argc, char **argv) {
    int rc=0;
    for (int i=1;i<argc;i++) {
        int fd=open(argv[i],O_WRONLY|O_CREAT|O_NOCTTY,0666);
        if(fd<0){perror(argv[i]);rc=1;continue;} close(fd);
        utimes(argv[i],NULL);
    }
    return rc;
}

/* ---- stat ---- */
int applet_stat(int argc, char **argv) {
    int rc=0;
    for (int i=1;i<argc;i++) {
        struct stat st; if(lstat(argv[i],&st)<0){perror(argv[i]);rc=1;continue;}
        char tmbuf[64]; struct tm *tm=localtime(&st.st_mtime); strftime(tmbuf,sizeof(tmbuf),"%Y-%m-%d %H:%M:%S",tm);
        printf("  File: %s\n  Size: %lld\t\tBlocks: %lld\tIO Block: %ld\n",
               argv[i],(long long)st.st_size,(long long)st.st_blocks,(long)st.st_blksize);
        printf("  Inode: %lu\tLinks: %lu\n",(unsigned long)st.st_ino,(unsigned long)st.st_nlink);
        printf("  Mode: %04o\tUid: %u\tGid: %u\n",(unsigned)(st.st_mode&07777),(unsigned)st.st_uid,(unsigned)st.st_gid);
        printf("  Modify: %s\n", tmbuf);
    }
    return rc;
}

/* ---- printf applet ---- */
int applet_printf(int argc, char **argv) {
    extern int builtin_printf_fn(int,char**);
    return builtin_printf_fn(argc,argv);
}

/* ---- test applet ---- */
int applet_test(int argc, char **argv) {
    extern int builtin_test(int,char**);
    return builtin_test(argc,argv);
}

/* ---- true / false ---- */
int applet_true(int argc, char **argv) { (void)argc;(void)argv; return 0; }
int applet_false(int argc, char **argv){ (void)argc;(void)argv; return 1; }

/* ---- grep ---- */
int applet_grep(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr,"grep: usage: grep [options] pattern [file...]\n"); return 2; }
    int i=1, invert=0, ignore_case=0, quiet=0, count_only=0, line_num=0, fixed=0, recursive=0;
    for (;i<argc&&argv[i][0]=='-';i++) {
        if(strcmp(argv[i],"--")==0){i++;break;}
        for(char *p=argv[i]+1;*p;p++){
            if(*p=='v') invert=1; if(*p=='i') ignore_case=1; if(*p=='q') quiet=1;
            if(*p=='c') count_only=1; if(*p=='n') line_num=1; if(*p=='F') fixed=1;
            if(*p=='r'||*p=='R') recursive=1; if(*p=='l'){quiet=1;}
        }
    }
    if (i >= argc) { fprintf(stderr,"grep: missing pattern\n"); return 2; }
    const char *pat = argv[i++];
    regex_t re; int re_flags = REG_EXTENDED|(ignore_case?REG_ICASE:0);
    int has_re = !fixed && regcomp(&re, pat, re_flags) == 0;
    int total_match=0, rc=1;
    char *files[1024]; int nfiles=0;
    for (;i<argc;i++) files[nfiles++]=argv[i];
    if (nfiles==0) files[nfiles++]=(char*)"-";
    for (int fi=0;fi<nfiles;fi++) {
        FILE *f=(strcmp(files[fi],"-")==0)?stdin:fopen(files[fi],"r");
        if(!f){perror(files[fi]);rc=2;continue;}
        char line[4096]; int lno=0, cnt=0;
        while(fgets(line,sizeof(line),f)) {
            lno++;
            int mlen=strlen(line); if(mlen>0&&line[mlen-1]=='\n') line[mlen-1]='\0';
            int match;
            if (fixed) {
                match = ignore_case ? (strcasestr(line,pat)!=NULL) : (strstr(line,pat)!=NULL);
            } else {
                match = has_re && (regexec(&re,line,0,NULL,0)==0);
            }
            if (invert) match=!match;
            if (match) {
                cnt++; total_match++;
                if (!quiet&&!count_only) {
                    if(nfiles>1) printf("%s:",files[fi]);
                    if(line_num) printf("%d:",lno);
                    printf("%s\n",line);
                }
            }
        }
        if (count_only) { if(nfiles>1) printf("%s:",files[fi]); printf("%d\n",cnt); }
        if (f!=stdin) fclose(f);
    }
    if (has_re) regfree(&re);
    return total_match>0?0:1;
}

/* ---- sed ---- */
int applet_sed(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr,"sed: usage: sed [-n] 's/pat/repl/flags' [file]\n"); return 1; }
    int i=1, silent=0;
    if (strcmp(argv[i],"-n")==0) { silent=1; i++; }
    if (i >= argc) return 1;
    const char *script = argv[i++];
    FILE *f = (i<argc) ? fopen(argv[i],"r") : stdin;
    if (!f) { perror(argv[i]); return 1; }
    char line[4096];
    while (fgets(line,sizeof(line),f)) {
        int mlen=strlen(line); int has_nl=(mlen>0&&line[mlen-1]=='\n');
        if(has_nl) line[mlen-1]='\0';
        if (script[0]=='s') {
            char delim=script[1];
            char pat[512]={0},repl[512]={0};
            const char *p=script+2;
            char *d=pat;
            while(*p&&*p!=delim) *d++=*p++;
            if(*p) p++;
            d=repl; while(*p&&*p!=delim) *d++=*p++;
            int global=0; if(*p) p++; if(*p=='g') global=1;
            regex_t re; if(regcomp(&re,pat,REG_EXTENDED)!=0){fputs(line,stdout);if(has_nl)putchar('\n');continue;}
            regmatch_t m;
            char out[4096]={0}; int ol=0; const char *lp=line;
            int first=1;
            while((first||global)&&regexec(&re,lp,1,&m,0)==0) {
                first=0;
                int pre=m.rm_so;
                strncat(out+ol,lp,pre); ol+=pre;
                strcat(out+ol,repl); ol+=strlen(repl);
                lp+=m.rm_eo; if(m.rm_eo==m.rm_so) { if(*lp) out[ol++]=*lp++; else break; }
            }
            strcat(out+ol,lp);
            if(!silent) { fputs(out,stdout); if(has_nl) putchar('\n'); }
            regfree(&re);
        } else if (script[0]=='p') {
            fputs(line,stdout); if(has_nl) putchar('\n');
            if(!silent) { fputs(line,stdout); if(has_nl) putchar('\n'); }
        } else {
            if(!silent){fputs(line,stdout);if(has_nl)putchar('\n');}
        }
    }
    if(f!=stdin) fclose(f);
    return 0;
}

/* ---- cut ---- */
int applet_cut(int argc, char **argv) {
    int i=1; char delim='\t'; char *fields=NULL; int bytes=0;
    for(;i<argc&&argv[i][0]=='-';i++) {
        if(argv[i][1]=='d'&&argv[i][2]) delim=argv[i][2];
        else if(argv[i][1]=='d'&&i+1<argc) delim=argv[++i][0];
        else if(argv[i][1]=='f'&&argv[i][2]) fields=argv[i]+2;
        else if(argv[i][1]=='f'&&i+1<argc) fields=argv[++i];
        else if(argv[i][1]=='b'&&argv[i][2]) { fields=argv[i]+2; bytes=1; }
    }
    if (!fields) return 0;
    FILE *f=(i<argc)?fopen(argv[i],"r"):stdin;
    if(!f){perror(argv[i]);return 1;}
    char line[4096];
    while(fgets(line,sizeof(line),f)) {
        int mlen=strlen(line); if(mlen>0&&line[mlen-1]=='\n') line[mlen-1]='\0';
        if (bytes) {
            int start=atoi(fields)-1; printf("%c\n",start<(int)strlen(line)?line[start]:'?');
            continue;
        }
        char **cols=NULL; int ncols=0;
        char *tmp=xstrdup(line), *tok, *rest=tmp;
        while((tok=strsep(&rest,(char[]){delim,0}))!=NULL) {
            cols=xrealloc(cols,(ncols+1)*sizeof(char*));
            cols[ncols++]=tok;
        }
        char *fspec=xstrdup(fields), *fp, *frest=fspec;
        int first=1;
        while((fp=strsep(&frest,","))!=NULL) {
            char *dash=strchr(fp,'-');
            int s,e;
            if(dash){*dash='\0';s=atoi(fp);e=dash[1]?atoi(dash+1):ncols;}
            else s=e=atoi(fp);
            for(int j=s;j<=e&&j<=ncols;j++) {
                if(!first) putchar(delim); first=0;
                fputs(cols[j-1],stdout);
            }
        }
        putchar('\n');
        free(fspec); free(cols); free(tmp);
    }
    if(f!=stdin) fclose(f);
    return 0;
}

/* ---- tr ---- */
static int tr_expand(const char *s, unsigned char *out) {
    int n = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (s[1] == '-' && s[2] != '\0') {
            unsigned char lo = c, hi = (unsigned char)s[2];
            if (lo <= hi) {
                for (unsigned char k = lo; k <= hi; k++) {
                    if (n < 256) out[n++] = k;
                }
            } else {
                if (n < 256) out[n++] = lo;
                if (n < 256) out[n++] = '-';
                if (n < 256) out[n++] = hi;
            }
            s += 3;
        } else {
            if (n < 256) out[n++] = c;
            s++;
        }
    }
    return n;
}

int applet_tr(int argc, char **argv) {
    int del=0, squeeze=0; int i=1;
    for(;i<argc&&argv[i][0]=='-';i++) {
        for(char *p=argv[i]+1;*p;p++) { if(*p=='d') del=1; if(*p=='s') squeeze=1; }
    }
    if (i >= argc) return 1;
    const char *set1=argv[i++], *set2=(i<argc)?argv[i]:NULL;
    unsigned char exp1[256], exp2[256];
    int n1 = tr_expand(set1, exp1);
    int n2 = set2 ? tr_expand(set2, exp2) : 0;
    unsigned char from[256]={0}, to[256]={0};
    for(int j=0;j<n1;j++) {
        from[exp1[j]]=1;
        if(!del&&set2) to[exp1[j]]=(j<n2)?exp2[j]:exp2[n2-1];
    }
    int c, last=-1;
    while((c=getchar())!=EOF) {
        if(del&&from[c]) continue;
        if(!del&&from[c]&&set2) c=to[c];
        if(squeeze&&set2&&c==last) continue;
        last=c; putchar(c);
    }
    return 0;
}

/* ---- wc ---- */
int applet_wc(int argc, char **argv) {
    int do_l=0,do_w=0,do_c=0;
    int i=1;
    for(;i<argc&&argv[i][0]=='-';i++) {
        for(char *p=argv[i]+1;*p;p++){if(*p=='l')do_l=1;if(*p=='w')do_w=1;if(*p=='c')do_c=1;}
    }
    if(!do_l&&!do_w&&!do_c) do_l=do_w=do_c=1;
    if(i>=argc) {
        long lines=0,words=0,chars=0; int in_word=0; int c;
        while((c=getchar())!=EOF){chars++;if(c=='\n')lines++;if(isspace(c)){in_word=0;}else if(!in_word){in_word=1;words++;}}
        if(do_l)printf("%ld ",lines); if(do_w)printf("%ld ",words); if(do_c)printf("%ld",chars); putchar('\n');
        return 0;
    }
    long tl=0,tw=0,tc=0; int rc=0;
    for(;i<argc;i++) {
        FILE *f=fopen(argv[i],"r"); if(!f){perror(argv[i]);rc=1;continue;}
        long lines=0,words=0,chars=0; int in_word=0; int c;
        while((c=fgetc(f))!=EOF){chars++;if(c=='\n')lines++;if(isspace(c)){in_word=0;}else if(!in_word){in_word=1;words++;}}
        tl+=lines;tw+=words;tc+=chars;
        if(do_l)printf("%ld ",lines); if(do_w)printf("%ld ",words); if(do_c)printf("%ld ",chars);
        printf("%s\n",argv[i]); fclose(f);
    }
    if(argc-i>1){if(do_l)printf("%ld ",tl);if(do_w)printf("%ld ",tw);if(do_c)printf("%ld ",tc);printf("total\n");}
    return rc;
}

/* ---- head ---- */
int applet_head(int argc, char **argv) {
    int n=10, i=1;
    if(i<argc&&argv[i][0]=='-'&&argv[i][1]=='n') { n=atoi(argv[i]+2?argv[i]+2:argv[++i]); i++; }
    if(i<argc&&argv[i][0]=='-'&&isdigit((unsigned char)argv[i][1])) { n=atoi(argv[i]+1); i++; }
    FILE *f=(i<argc)?fopen(argv[i],"r"):stdin;
    if(!f){perror(argv[i]);return 1;}
    char line[4096]; int cnt=0;
    while(cnt<n&&fgets(line,sizeof(line),f)) { fputs(line,stdout); cnt++; }
    if(f!=stdin) fclose(f);
    return 0;
}

/* ---- tail ---- */
int applet_tail(int argc, char **argv) {
    int n=10, follow=0, i=1;
    for(;i<argc&&argv[i][0]=='-';i++) {
        if(argv[i][1]=='n') { n=atoi(argv[i][2]?argv[i]+2:argv[++i]); }
        else if(argv[i][1]=='f') follow=1;
        else if(isdigit((unsigned char)argv[i][1])) n=atoi(argv[i]+1);
    }
    FILE *f=(i<argc)?fopen(argv[i],"r"):stdin;
    if(!f){perror(argv[i]);return 1;}
    char **buf=xmalloc(n*sizeof(char*)); memset(buf,0,n*sizeof(char*));
    int pos=0; char line[4096];
    while(fgets(line,sizeof(line),f)) { free(buf[pos]); buf[pos]=xstrdup(line); pos=(pos+1)%n; }
    for(int j=0;j<n;j++) { int k=(pos+j)%n; if(buf[k]){fputs(buf[k],stdout);free(buf[k]);} }
    free(buf);
    if(follow&&f!=stdin) {
        while(1) { while(fgets(line,sizeof(line),f)) fputs(line,stdout); fflush(stdout); usleep(250000); }
    }
    if(f!=stdin) fclose(f);
    return 0;
}

/* ---- sort ---- */
static struct { int rev; int num; } sort_ctx;
static int cmp_lines(const void *a, const void *b) {
    const char *x=*(const char**)a, *y=*(const char**)b;
    int r = sort_ctx.num ? (atol(x)-atol(y)>0?1:atol(x)-atol(y)<0?-1:0) : strcmp(x,y);
    return sort_ctx.rev ? -r : r;
}
int applet_sort(int argc, char **argv) {
    int i=1, rev=0, num=0, unique=0;
    for(;i<argc&&argv[i][0]=='-';i++) {
        for(char *p=argv[i]+1;*p;p++){if(*p=='r')rev=1;if(*p=='n')num=1;if(*p=='u')unique=1;}
    }
    char **lines=NULL; int nl=0,cap=0;
    FILE *f=(i<argc)?fopen(argv[i],"r"):stdin;
    if(!f){perror(argv[i]);return 1;}
    char line[4096];
    while(fgets(line,sizeof(line),f)){if(nl>=cap){cap=cap?cap*2:64;lines=xrealloc(lines,cap*sizeof(char*));}lines[nl++]=xstrdup(line);}
    if(f!=stdin) fclose(f);
    sort_ctx.rev = rev; sort_ctx.num = num;
    qsort(lines, nl, sizeof(char*), cmp_lines);
    const char *last=NULL;
    for(int j=0;j<nl;j++) {
        if(unique&&last&&strcmp(lines[j],last)==0){free(lines[j]);continue;}
        fputs(lines[j],stdout); last=lines[j]; free(lines[j]);
    }
    free(lines); return 0;
}

/* ---- uniq ---- */
int applet_uniq(int argc, char **argv) {
    int count=0,dup_only=0,uniq_only=0,i=1;
    for(;i<argc&&argv[i][0]=='-';i++){
        for(char *p=argv[i]+1;*p;p++){if(*p=='c')count=1;if(*p=='d')dup_only=1;if(*p=='u')uniq_only=1;}
    }
    FILE *f=(i<argc)?fopen(argv[i],"r"):stdin;
    FILE *o=(i+1<argc)?fopen(argv[i+1],"w"):stdout;
    if(!f){perror(argv[i]);return 1;}
    char prev[4096]={0},line[4096]; int cnt=0;
    while(fgets(line,sizeof(line),f)){
        if(strcmp(line,prev)==0){cnt++;continue;}
        if(cnt>0){
            int pr=(!dup_only&&!uniq_only)||(dup_only&&cnt>1)||(uniq_only&&cnt==1);
            if(pr){if(count)fprintf(o,"%7d ",cnt);fputs(prev,o);}
        }
        strncpy(prev,line,sizeof(prev)-1); cnt=1;
    }
    if(cnt>0){int pr=(!dup_only&&!uniq_only)||(dup_only&&cnt>1)||(uniq_only&&cnt==1);if(pr){if(count)fprintf(o,"%7d ",cnt);fputs(prev,o);}}
    if(f!=stdin)fclose(f); if(o!=stdout)fclose(o);
    return 0;
}

/* ---- tee ---- */
int applet_tee(int argc, char **argv) {
    int append=0,i=1;
    for(;i<argc&&argv[i][0]=='-';i++) for(char *p=argv[i]+1;*p;p++) if(*p=='a') append=1;
    int nfiles=argc-i; FILE **files=xmalloc(nfiles*sizeof(FILE*));
    for(int j=0;j<nfiles;j++) { files[j]=fopen(argv[i+j],append?"a":"w"); if(!files[j]) perror(argv[i+j]); }
    char buf[4096]; ssize_t n;
    while((n=read(0,buf,sizeof(buf)))>0) {
        write(1,buf,n);
        for(int j=0;j<nfiles;j++) if(files[j]) fwrite(buf,1,n,files[j]);
    }
    for(int j=0;j<nfiles;j++) if(files[j]) fclose(files[j]);
    free(files); return 0;
}

/* ---- find ---- */
static void do_find(const char *base) {
    char path[PATH_MAX];
    DIR *d=opendir(base); if(!d) return;
    struct dirent *e;
    while((e=readdir(d))) {
        if(strcmp(e->d_name,".")==0||strcmp(e->d_name,"..")==0) continue;
        snprintf(path,sizeof(path),"%s/%s",base,e->d_name);
        printf("%s\n",path);
        if(e->d_type==DT_DIR) do_find(path);
    }
    closedir(d);
}
int applet_find(int argc, char **argv) {
    const char *start=(argc>1&&argv[1][0]!='.'&&argv[1][0]!='/')?NULL:((argc>1)?argv[1]:NULL);
    if(!start) start=".";
    printf("%s\n",start);
    do_find(start);
    return 0;
}

/* ---- xargs ---- */
int applet_xargs(int argc, char **argv) {
    const char *cmd=(argc>1)?argv[1]:"echo";
    char **args=xmalloc(128*sizeof(char*));
    args[0]=(char*)cmd; int na=1;
    char word[1024]; int wi=0; int c;
    while((c=getchar())!=EOF) {
        if(isspace(c)) {
            if(wi>0){word[wi]='\0';args[na++]=xstrdup(word);wi=0;}
            if(na>=126){args[na]=NULL;execvp(cmd,args);na=1;}
        } else word[wi++]=c;
    }
    if(wi>0){word[wi]='\0';args[na++]=xstrdup(word);}
    if(na>1){args[na]=NULL;pid_t pid=fork();if(pid==0){execvp(cmd,args);_exit(127);}int st;waitpid(pid,&st,0);}
    return 0;
}

/* ---- mount / umount ---- */
int applet_mount(int argc, char **argv) {
    if(argc==1){
        FILE *f=fopen("/proc/mounts","r"); if(!f)f=fopen("/etc/mtab","r"); if(!f)return 1;
        char line[256]; while(fgets(line,sizeof(line),f)) fputs(line,stdout); fclose(f); return 0;
    }
    unsigned long flags=0; const char *type=NULL,*opts=NULL;
    int i=1;
    for(;i<argc&&argv[i][0]=='-';i++){
        for(char *p=argv[i]+1;*p;p++){
            if(*p=='t'&&i+1<argc) type=argv[++i];
            if(*p=='o'&&i+1<argc) opts=argv[++i];
            if(*p=='r') flags|=MS_RDONLY;
            if(*p=='n') ;
        }
    }
    if(i+1>=argc){fprintf(stderr,"mount: missing args\n");return 1;}
    const char *dev=argv[i],*mnt=argv[i+1];
    if(mount(dev,mnt,type?type:"",flags,opts)<0){perror("mount");return 1;}
    return 0;
}
int applet_umount(int argc, char **argv) {
    if(argc<2){fprintf(stderr,"umount: missing target\n");return 1;}
    int i=1,lazy=0;
    for(;i<argc&&argv[i][0]=='-';i++) for(char *p=argv[i]+1;*p;p++) if(*p=='l')lazy=1;
    int flags=lazy?MNT_DETACH:0; int rc=0;
    for(;i<argc;i++) if(umount2(argv[i],flags)<0){perror(argv[i]);rc=1;}
    return rc;
}

/* ---- sync ---- */
int applet_sync(int argc, char **argv) { (void)argc;(void)argv; sync(); return 0; }

/* ---- ps ---- */
int applet_ps(int argc, char **argv) {
    (void)argc;(void)argv;
    DIR *d=opendir("/proc"); if(!d){perror("/proc");return 1;}
    printf("%-6s %-8s %-6s %s\n","PID","STAT","TIME","COMMAND");
    struct dirent *e;
    while((e=readdir(d))){
        if(!isdigit((unsigned char)e->d_name[0])) continue;
        char path[256],stat_str[512]={0},cmdline[512]={0};
        snprintf(path,sizeof(path),"/proc/%s/stat",e->d_name);
        FILE *f=fopen(path,"r"); if(!f) continue;
        fgets(stat_str,sizeof(stat_str),f); fclose(f);
        snprintf(path,sizeof(path),"/proc/%s/cmdline",e->d_name);
        f=fopen(path,"r"); if(f){int n=fread(cmdline,1,sizeof(cmdline)-1,f);for(int i=0;i<n;i++)if(!cmdline[i])cmdline[i]=' ';fclose(f);}
        int pid,ppid; char comm[64]={0},state=' ';
        sscanf(stat_str,"%d %63s %c %d",&pid,comm,&state,&ppid);
        printf("%-6d %-8c %-6s %s\n",pid,state,"-",cmdline[0]?cmdline:comm);
    }
    closedir(d); return 0;
}

/* ---- kill ---- */
int applet_kill(int argc, char **argv) {
    if(argc<2){fprintf(stderr,"kill: usage: kill [-SIG] pid...\n");return 1;}
    int i=1; int sig=SIGTERM;
    if(argv[i][0]=='-'){
        if(isdigit((unsigned char)argv[i][1])) sig=atoi(argv[i]+1);
        else {
            const char *names[]={"HUP","INT","QUIT","ILL","ABRT","FPE","KILL","SEGV","PIPE","ALRM","TERM","USR1","USR2",NULL};
            int sigs[]={1,2,3,4,6,8,9,11,13,14,15,10,12};
            for(int j=0;names[j];j++) if(strcasecmp(argv[i]+1,names[j])==0){sig=sigs[j];break;}
        }
        i++;
    }
    int rc=0;
    for(;i<argc;i++) if(kill(atoi(argv[i]),sig)<0){perror(argv[i]);rc=1;}
    return rc;
}

/* ---- env ---- */
int applet_env(int argc, char **argv) {
    int i=1; int clear=0;
    if(i<argc&&strcmp(argv[i],"-i")==0){clear=1;i++;}
    if(i<argc&&strcmp(argv[i],"-")==0){clear=1;i++;}
    if(clear) { environ=(char*[]){NULL}; }
    for(;i<argc&&strchr(argv[i],'=');i++) putenv(argv[i]);
    if(i<argc) { execvp(argv[i],argv+i); perror(argv[i]); return 127; }
    for(char **e=environ;*e;e++) printf("%s\n",*e);
    return 0;
}

/* ---- uname ---- */
int applet_uname(int argc, char **argv) {
    struct utsname u; uname(&u);
    int all=0,sys=0,node=0,rel=0,ver=0,mach=0;
    if(argc==1) sys=1;
    for(int i=1;i<argc;i++) for(char *p=argv[i]+1;*p;p++){
        if(*p=='a')all=1;if(*p=='s')sys=1;if(*p=='n')node=1;
        if(*p=='r')rel=1;if(*p=='v')ver=1;if(*p=='m')mach=1;
    }
    if(all){sys=node=rel=ver=mach=1;}
    if(sys)  printf("%s ",u.sysname);
    if(node) printf("%s ",u.nodename);
    if(rel)  printf("%s ",u.release);
    if(ver)  printf("%s ",u.version);
    if(mach) printf("%s ",u.machine);
    putchar('\n'); return 0;
}

/* ---- hostname ---- */
int applet_hostname(int argc, char **argv) {
    if(argc>1) { sethostname(argv[1],strlen(argv[1])); return 0; }
    char h[256]; gethostname(h,sizeof(h)); printf("%s\n",h); return 0;
}

/* ---- pwd ---- */
int applet_pwd(int argc, char **argv) {
    (void)argc;(void)argv;
    char cwd[PATH_MAX]; getcwd(cwd,sizeof(cwd)); printf("%s\n",cwd); return 0;
}

/* ---- date ---- */
int applet_date(int argc, char **argv) {
    time_t t=time(NULL); struct tm *tm=localtime(&t);
    const char *fmt="%a %b %e %H:%M:%S %Z %Y";
    for(int i=1;i<argc;i++) if(argv[i][0]=='+') fmt=argv[i]+1;
    char buf[256]; strftime(buf,sizeof(buf),fmt,tm); printf("%s\n",buf);
    return 0;
}

/* ---- sleep ---- */
int applet_sleep(int argc, char **argv) {
    if(argc<2){fprintf(stderr,"sleep: missing operand\n");return 1;}
    double s=strtod(argv[1],NULL);
    struct timespec ts; ts.tv_sec=(long)s; ts.tv_nsec=(long)((s-(long)s)*1e9);
    nanosleep(&ts,NULL); return 0;
}

/* ---- seq ---- */
int applet_seq(int argc, char **argv) {
    double start=1,step=1,end;
    if(argc==2) end=atof(argv[1]);
    else if(argc==3){start=atof(argv[1]);end=atof(argv[2]);}
    else{start=atof(argv[1]);step=atof(argv[2]);end=atof(argv[3]);}
    for(double v=start;(step>0?v<=end:v>=end);v+=step) printf("%g\n",v);
    return 0;
}

/* ---- yes ---- */
int applet_yes(int argc, char **argv) {
    const char *s=(argc>1)?argv[1]:"y";
    for(;;) printf("%s\n",s);
}

/* ---- id / whoami ---- */
int applet_id(int argc, char **argv) {
    (void)argc;(void)argv;
    uid_t uid=getuid(); gid_t gid=getgid();
    struct passwd *pw=getpwuid(uid); struct group *gr=getgrgid(gid);
    printf("uid=%u(%s) gid=%u(%s)\n",uid,pw?pw->pw_name:"?",gid,gr?gr->gr_name:"?");
    return 0;
}
int applet_whoami(int argc, char **argv) {
    (void)argc;(void)argv;
    struct passwd *pw=getpwuid(getuid()); printf("%s\n",pw?pw->pw_name:"?"); return 0;
}

/* ---- basename / dirname ---- */
int applet_basename(int argc, char **argv) {
    if(argc<2){fprintf(stderr,"basename: missing operand\n");return 1;}
    char *p=strrchr(argv[1],'/'); printf("%s\n",p?p+1:argv[1]); return 0;
}
int applet_dirname(int argc, char **argv) {
    if(argc<2){fprintf(stderr,"dirname: missing operand\n");return 1;}
    char *s=xstrdup(argv[1]); char *p=strrchr(s,'/');
    if(p&&p!=s){*p='\0';printf("%s\n",s);}
    else if(p==s) printf("/\n");
    else printf(".\n");
    free(s); return 0;
}

/* ---- readlink / realpath ---- */
int applet_readlink(int argc, char **argv) {
    if(argc<2)return 1;
    int i=1; if(strcmp(argv[i],"-f")==0)i++;
    char buf[PATH_MAX];
    if(argv[i][0]&&readlink(argv[i],buf,sizeof(buf)-1)>=0){buf[PATH_MAX-1]='\0';printf("%s\n",buf);}
    else if(argv[i-1][0]=='-'){realpath(argv[i],buf);printf("%s\n",buf);}
    else{perror(argv[i]);return 1;}
    return 0;
}
int applet_realpath(int argc, char **argv) {
    if(argc<2)return 1;
    char buf[PATH_MAX]; realpath(argv[1],buf); printf("%s\n",buf); return 0;
}

/* ---- which ---- */
int applet_which(int argc, char **argv) {
    int rc=0;
    const char *PATH=getenv("PATH"); if(!PATH)PATH="/bin:/usr/bin";
    for(int i=1;i<argc;i++){
        char path[PATH_MAX]; char *pp=xstrdup(PATH),*tok,*rest=pp; int found=0;
        while((tok=strsep(&rest,":"))!=NULL){
            snprintf(path,sizeof(path),"%s/%s",tok,argv[i]);
            if(access(path,X_OK)==0){printf("%s\n",path);found=1;break;}
        }
        free(pp); if(!found){fprintf(stderr,"%s: not found\n",argv[i]);rc=1;}
    }
    return rc;
}

/* ---- dd ---- */
int applet_dd(int argc, char **argv) {
    const char *inf=NULL,*outf=NULL; size_t bs=512,count=0,skip=0,seek=0;
    for(int i=1;i<argc;i++){
        if(strncmp(argv[i],"if=",3)==0) inf=argv[i]+3;
        else if(strncmp(argv[i],"of=",3)==0) outf=argv[i]+3;
        else if(strncmp(argv[i],"bs=",3)==0) bs=strtoul(argv[i]+3,NULL,0);
        else if(strncmp(argv[i],"count=",6)==0) count=strtoul(argv[i]+6,NULL,0);
        else if(strncmp(argv[i],"skip=",5)==0) skip=strtoul(argv[i]+5,NULL,0);
        else if(strncmp(argv[i],"seek=",5)==0) seek=strtoul(argv[i]+5,NULL,0);
    }
    int in=inf?open(inf,O_RDONLY):0;
    int out=outf?open(outf,O_WRONLY|O_CREAT|O_TRUNC,0666):1;
    if(in<0){perror(inf);return 1;} if(out<0){perror(outf);return 1;}
    if(skip) lseek(in, skip*bs, SEEK_SET);
    if(seek) lseek(out, seek*bs, SEEK_SET);
    char *buf=xmalloc(bs); size_t blks=0,bytes_in=0;
    ssize_t n;
    while((n=read(in,buf,bs))>0){bytes_in+=n;write(out,buf,n);blks++;if(count&&blks>=count)break;}
    free(buf);
    fprintf(stderr,"%zu+0 records in\n%zu+0 records out\n%zu bytes copied\n",blks,blks,bytes_in);
    if(in!=0)close(in); if(out!=1)close(out); return 0;
}

/* ---- hexdump ---- */
int applet_hexdump(int argc, char **argv) {
    int i=1; int canonical=0;
    for(;i<argc&&argv[i][0]=='-';i++) for(char *p=argv[i]+1;*p;p++) if(*p=='C') canonical=1;
    FILE *f=(i<argc)?fopen(argv[i],"r"):stdin;
    if(!f){perror(argv[i]);return 1;}
    unsigned char buf[16]; size_t off=0; int n;
    while((n=fread(buf,1,sizeof(buf),f))>0){
        printf("%08zx  ",off);
        for(int j=0;j<16;j++){if(j<n)printf("%02x ",buf[j]);else printf("   ");if(j==7)printf(" ");}
        if(canonical){printf(" |");for(int j=0;j<n;j++)putchar(isprint(buf[j])?buf[j]:'.');printf("|");}
        putchar('\n'); off+=n;
    }
    if(f!=stdin)fclose(f); return 0;
}

/* ---- reboot / shutdown / poweroff ---- */
int applet_reboot(int argc, char **argv) {
    (void)argc;(void)argv;
    sync(); sysrq('b');
    reboot(RB_AUTOBOOT); return 0;
}
int applet_poweroff(int argc, char **argv) {
    (void)argc;(void)argv;
    sync(); sysrq('o');
    reboot(RB_POWER_OFF); return 0;
}
int applet_shutdown(int argc, char **argv) {
    int do_r=0, do_h=0;
    for(int i=1;i<argc;i++){if(strcmp(argv[i],"-r")==0)do_r=1;if(strcmp(argv[i],"-h")==0)do_h=1;}
    if(do_r) return applet_reboot(0,NULL);
    if(do_h) return applet_poweroff(0,NULL);
    kill(1,SIGTERM);
    return 0;
}

/* ---- dmesg ---- */
int applet_dmesg(int argc, char **argv) {
    (void)argc;(void)argv;
    int len=klogctl(10,NULL,0); if(len<0){perror("klogctl");return 1;}
    char *buf=xmalloc(len+1);
    klogctl(3,buf,len); buf[len]='\0';
    fputs(buf,stdout); free(buf); return 0;
}

/* ---- free ---- */
int applet_free(int argc, char **argv) {
    int human=0;
    for(int i=1;i<argc;i++) if(strcmp(argv[i],"-h")==0) human=1;
    struct sysinfo si; sysinfo(&si);
    printf("%-10s %10s %10s %10s\n","","total","used","free");
    long long tot=si.totalram*si.mem_unit, fr=si.freeram*si.mem_unit, us=tot-fr;
    if(human){printf("%-10s ","Mem:"); print_size(tot);printf(" ");print_size(us);printf(" ");print_size(fr);putchar('\n');}
    else printf("%-10s %10lld %10lld %10lld\n","Mem:",tot/1024,us/1024,fr/1024);
    return 0;
}

/* ---- df ---- */
int applet_df(int argc, char **argv) {
    int human=0;
    for(int i=1;i<argc;i++) if(strcmp(argv[i],"-h")==0) human=1;
    printf("%-20s %10s %10s %10s %6s %s\n","Filesystem","1K-blocks","Used","Available","Use%","Mounted on");
    FILE *f=fopen("/proc/mounts","r"); if(!f) return 1;
    char line[512];
    while(fgets(line,sizeof(line),f)){
        char dev[128],mnt[128]; if(sscanf(line,"%127s %127s",dev,mnt)!=2) continue;
        struct statvfs sv; if(statvfs(mnt,&sv)!=0) continue;
        long long tot=(long long)sv.f_blocks*sv.f_frsize/1024;
        long long fr=(long long)sv.f_bfree*sv.f_frsize/1024;
        long long us=tot-fr;
        int pct=tot?((int)(us*100/tot)):0;
        printf("%-20s %10lld %10lld %10lld %5d%% %s\n",dev,tot,us,fr,pct,mnt);
    }
    fclose(f); return 0;
}

/* ---- du ---- */
static struct { int human; int summarize; } du_ctx;
static void do_du(const char *p, long long *total) {
    struct stat st; if(lstat(p,&st)<0) return;
    *total+=st.st_blocks/2;
    if(S_ISDIR(st.st_mode)){
        DIR *d=opendir(p); if(!d) return; struct dirent *e;
        while((e=readdir(d))){
            if(strcmp(e->d_name,".")==0||strcmp(e->d_name,"..")==0) continue;
            char sub[PATH_MAX]; snprintf(sub,sizeof(sub),"%s/%s",p,e->d_name);
            long long sub_total=0; do_du(sub,&sub_total); *total+=sub_total;
            if(!du_ctx.summarize){if(du_ctx.human){print_size(sub_total*1024);printf("\t%s\n",sub);}else printf("%lld\t%s\n",sub_total,sub);}
        }
        closedir(d);
    }
}
int applet_du(int argc, char **argv) {
    int human=0,summarize=0; int i=1;
    for(;i<argc&&argv[i][0]=='-';i++) for(char *p=argv[i]+1;*p;p++){if(*p=='h')human=1;if(*p=='s')summarize=1;}
    const char *path=(i<argc)?argv[i]:".";
    du_ctx.human = human; du_ctx.summarize = summarize;
    long long total=0; do_du(path,&total);
    if(human){print_size(total*1024);printf("\t%s\n",path);}else printf("%lld\t%s\n",total,path);
    return 0;
}

/* ---- mkfifo / mknod ---- */
int applet_mkfifo(int argc, char **argv) {
    if(argc<2)return 1;
    int rc=0;
    for(int i=1;i<argc;i++) if(mkfifo(argv[i],0666)<0){perror(argv[i]);rc=1;}
    return rc;
}
int applet_mknod(int argc, char **argv) {
    if(argc<4){fprintf(stderr,"mknod: usage: mknod name type major minor\n");return 1;}
    mode_t mode=0666; dev_t dev=0;
    if(argv[2][0]=='b') mode|=S_IFBLK;
    else if(argv[2][0]=='c'||argv[2][0]=='u') mode|=S_IFCHR;
    else if(argv[2][0]=='p') { if(mkfifo(argv[1],mode)<0){perror(argv[1]);return 1;} return 0; }
    if(argc>=5) dev=makedev(atoi(argv[3]),atoi(argv[4]));
    if(mknod(argv[1],mode,dev)<0){perror(argv[1]);return 1;}
    return 0;
}

/* ---- tty / stty ---- */
int applet_tty(int argc, char **argv) {
    (void)argc;(void)argv;
    char *t=ttyname(0); printf("%s\n",t?t:"not a tty"); return t?0:1;
}
int applet_stty(int argc, char **argv) {
    (void)argc;(void)argv;
    struct termios t; tcgetattr(0,&t);
    printf("speed %lu baud; rows %u; columns %u\n",(unsigned long)cfgetispeed(&t),0,0);
    return 0;
}

/* ---- nohup / nice ---- */
int applet_nohup(int argc, char **argv) {
    if(argc<2){fprintf(stderr,"nohup: missing command\n");return 1;}
    signal(SIGHUP,SIG_IGN);
    execvp(argv[1],argv+1); perror(argv[1]); return 127;
}
int applet_nice(int argc, char **argv) {
    int n=10, i=1;
    if(i<argc&&argv[i][0]=='-'&&argv[i][1]=='n') { n=atoi(argv[i][2]?argv[i]+2:argv[++i]); i++; }
    if(i>=argc){fprintf(stderr,"nice: missing command\n");return 1;}
    nice(n); execvp(argv[i],argv+i); perror(argv[i]); return 127;
}
