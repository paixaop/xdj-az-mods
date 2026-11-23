/* SPDX-License-Identifier: GPL-3.0-only */
/* folder_copy.c: COPY a folder or a file in the Browse FOLDER view, PASTE it on the other USB.
 * #included by azmod.c after playlist_actions.c.
 *
 * A plain file copy: no library rows, no database write, no file contents read beyond the
 * bytes copied. The plan (folder_copy.h) checks every name and never overwrites. Then one
 * SCHED_IDLE worker:
 *   - checks the destination's free space for the whole plan (+ FC_SPARE) first;
 *   - leaves a marker naming the copy on the destination stick, so an interrupted copy's
 *     unfinished files are removed at the next mount (fc_mount_cleanup);
 *   - creates each folder, and writes each file as "<name>.azcp", fsyncs and renames it, so no
 *     reader ever sees half a file under the real name; a name that appeared meanwhile stops
 *     the copy (never overwritten);
 *   - stops when either stick is no longer mounted where it was.
 * The message thread starts it (fc_request), shows the progress and the result (fc_tick).
 * One copy at a time, and never beside the playlist copy (shared exclusion: cp_busy). */
#include <dirent.h>
#include <errno.h>
#include <sys/statvfs.h>
#include "folder_copy.h"

#define FC_ITEMS_CAP (FC_MAX_FILES + 512) /* files + the folders holding them */
#define FC_SPARE (16ull << 20)            /* free space kept on the destination */
#define FC_MARKER "/PIONEER/azspool/azfcopy-inprogress"
#define FC_BUF (1 << 16)

enum {
    FC_IDLE,
    FC_RUNNING,
    FC_DONE
};
static struct {
    int state;                                /* (atomic) */
    int done_n, total_n;                      /* (atomic) files copied / to copy */
    int rc;                                   /* written by the worker before FC_DONE */
    char src_mount[128], dst_mount[128];      /* set before the worker starts, then read-only */
    char src_rel[FC_PATH_MAX], dst_dir[FC_PATH_MAX];
    char name[96];                            /* the copied entry's name, for the messages */
    unsigned dst_media, dst_id;               /* where it lands, for the re-list */
    char landed[FC_PATH_MAX];                 /* where it landed (worker; read after FC_DONE) */
    int skipped;
} g_fc;

/* the real file system under a mount (ctx = the mount path) */
static int fc_abs(char *out, size_t cap, const char *mount, const char *rel) {
    int r = rel[0] ? snprintf(out, cap, "%s/%s", mount, rel) : snprintf(out, cap, "%s", mount);
    return r < 0 || (size_t) r >= cap ? -1 : 0;
}
static int fcr_stat(void *ctx, const char *rel, uint64_t *size) {
    char p[FC_PATH_MAX + 160];
    struct stat st;
    if (fc_abs(p, sizeof p, (const char *) ctx, rel) || lstat(p, &st) != 0)
        return 0;
    if (S_ISDIR(st.st_mode))
        return 2;
    if (!S_ISREG(st.st_mode))
        return 0;
    *size = (uint64_t) st.st_size;
    return 1;
}
static int fcr_list(void *ctx, const char *rel, int (*each)(void *, const char *), void *arg) {
    char p[FC_PATH_MAX + 160];
    if (fc_abs(p, sizeof p, (const char *) ctx, rel))
        return -1;
    DIR *d = opendir(p);
    if (!d)
        return -1;
    int result = 0;
    for (;;) {
        /* Callbacks can leave errno set, including through recursive walks. */
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) {
            result = errno ? -1 : 0;
            break;
        }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (each(arg, e->d_name))
            break;
    }
    closedir(d);
    return result;
}

static int fc_still_mounted(const char *mount) {
    struct stat a, b;
    char parent[160];
    snprintf(parent, sizeof parent, "%s/..", mount);
    /* a mount point: its device differs from its parent's */
    return stat(mount, &a) == 0 && stat(parent, &b) == 0 && a.st_dev != b.st_dev;
}

/* one file: <dst>.azcp, fsync, rename; 0, or <0 (the .azcp removed) */
static int fc_copy_file(const char *src, const char *dst, char *buf) {
    struct stat ds;
    if (lstat(dst, &ds) == 0)
        return -2; /* appeared after the plan: never overwritten */
    char tmp[FC_PATH_MAX + 200];
    if ((size_t) snprintf(tmp, sizeof tmp, "%s.azcp", dst) >= sizeof tmp)
        return -1;
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -3;
    int out = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (out < 0) {
        close(in);
        return -3;
    }
    ssize_t r;
    int ok = 1;
    while ((r = read(in, buf, FC_BUF)) > 0)
        if (write(out, buf, (size_t) r) != r) {
            ok = 0;
            break;
        }
    if (r < 0 || fsync(out) != 0)
        ok = 0;
    close(in);
    if (close(out) != 0)
        ok = 0;
    if (!ok || lstat(dst, &ds) == 0 || rename(tmp, dst) != 0) {
        unlink(tmp);
        return -4;
    }
    return 0;
}

static void *fc_worker(void *arg) {
    (void) arg;
    struct sched_param sp = {0};
    pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp);
    int rc = 0;
    struct fc_fs fs_src = {g_fc.src_mount, fcr_stat, fcr_list};
    struct fc_fs fs_dst = {g_fc.dst_mount, fcr_stat, fcr_list};
    struct fc_plan p = {calloc(FC_ITEMS_CAP, sizeof(struct fc_item)), 0, FC_ITEMS_CAP, 0, 0, 0, 0, 0};
    char *buf = malloc(FC_BUF);
    char marker[200];
    snprintf(marker, sizeof marker, "%s" FC_MARKER, g_fc.dst_mount);
    if (!p.items || !buf) {
        rc = -10;
        goto out;
    }
    if (fc_plan_make(&fs_src, &fs_dst, g_fc.src_rel, g_fc.dst_dir, &p) < 0) {
        rc = p.err == FC_E_FILES || p.err == FC_E_FULL || p.err == FC_E_ENTRIES ? -11
             : p.err == FC_E_DEPTH                                              ? -12
                                                                                : -13;
        goto out;
    }
    g_fc.skipped = p.skipped;
    if (p.n == 0)
        goto out; /* already there */
    snprintf(g_fc.landed, sizeof g_fc.landed, "%s", p.items[0].dst);
    struct statvfs vf;
    char dpath[200];
    snprintf(dpath, sizeof dpath, "%s", g_fc.dst_mount);
    if (statvfs(dpath, &vf) != 0 || (uint64_t) vf.f_bavail * vf.f_frsize < p.bytes + FC_SPARE) {
        rc = -14;
        goto out;
    }
    __atomic_store_n(&g_fc.total_n, p.files, __ATOMIC_RELEASE);
    /* the marker: the first destination path, so the next mount can clear its .azcp files */
    {
        char dir[200];
        snprintf(dir, sizeof dir, "%s/PIONEER/azspool", g_fc.dst_mount);
        mkdir(dir, 0777);
        FILE *m = fopen(marker, "we");
        if (!m) {
            rc = -15;
            goto out;
        }
        fprintf(m, "%s\n", p.items[0].dst);
        if (fclose(m) != 0) {
            rc = -15;
            goto out;
        }
    }
    for (int i = 0; i < p.n && !rc; i++) {
        if (!fc_still_mounted(g_fc.src_mount) || !fc_still_mounted(g_fc.dst_mount)) {
            rc = -16;
            break;
        }
        char s[FC_PATH_MAX + 160], d[FC_PATH_MAX + 160];
        if (fc_abs(s, sizeof s, g_fc.src_mount, p.items[i].src) ||
            fc_abs(d, sizeof d, g_fc.dst_mount, p.items[i].dst)) {
            rc = -13;
            break;
        }
        if (p.items[i].is_dir) {
            if (mkdir(d, 0777) != 0)
                rc = -17; /* the plan chose a free name: it must not exist now */
            continue;
        }
        int r = fc_copy_file(s, d, buf);
        if (r < 0)
            rc = r == -2 ? -17 : -18;
        else
            __atomic_add_fetch(&g_fc.done_n, 1, __ATOMIC_RELEASE);
    }
    if (!rc)
        unlink(marker); /* an error keeps it: the files are complete, only .azcp may remain */
out:
    free(p.items);
    free(buf);
    g_fc.rc = rc;
    __atomic_store_n(&g_fc.state, FC_DONE, __ATOMIC_RELEASE);
    return NULL;
}

static const char *fc_why(int rc) {
    switch (rc) {
    case -11: return "Too many files to copy";
    case -12: return "Folders nested too deep to copy";
    case -14: return "Not enough space on the other USB";
    case -15: return "The other USB cannot be written";
    case -16: return "A USB was removed: copy stopped";
    case -17: return "A name on the other USB changed: copy stopped";
    case -18: return "Copy failed: a file could not be written";
    default: return "Copy failed";
    }
}

/* After a copy: EP147 keeps each stick's FOLDER lists in its ListCacheCollector. The message
 * thread asks (media + the destination folder's entry id); that stick's library thread drops
 * the folder's list and the FOLDER hierarchy at its next message boundary. */
#define FC_DROP_FOLDER EP_1244008 /* ListCacheCollector::removeFolderListCache(u32 folder) */
#define FC_DROP_HIER EP_12441d8   /* ListCacheCollector::removeHierarchyCache(u16 category) */
#define FC_DIGGER_VT EP_2847138     /* music_library::FolderEntryDigger */
#define FC_DIGGER_REREAD EP_11755a0 /* FolderEntryDigger: re-read one analysed folder (id) */
#define FC_DB_DIGGER 0x40           /* the stick's database object -> its FolderEntryDigger */
static int fc_vt_is(uintptr_t o, uintptr_t vt) {
    uintptr_t v = 0;
    return vt && own_q(o, &v) && v == vt;
}
static unsigned g_fcr_media, g_fcr_id; /* (atomic) media 0 = nothing pending */
static void fc_refresh_service(void *server) {
    unsigned want = __atomic_load_n(&g_fcr_media, __ATOMIC_ACQUIRE);
    if (!want)
        return;
    uintptr_t svc = 0, lib = 0, db = 0, cache = 0;
    unsigned media = 0;
    if (mlserver_chain(server, &svc, &lib, &db, &media) || media != want ||
        !own_q(svc + INFOUPD_CACHE, &cache) || !cache)
        return;
    if (!__atomic_compare_exchange_n(&g_fcr_media, &want, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    unsigned id = __atomic_load_n(&g_fcr_id, __ATOMIC_ACQUIRE);
    /* EP147 reads a folder from disk once and keeps its entries (FolderEntryDigger, db+0x40);
     * its own re-read brings in the copied files, then the cached list and hierarchy are dropped
     * so the next look builds them from it. */
    uintptr_t dig = 0;
    int reread = -2;
    if (own_q(db + FC_DB_DIGGER, &dig) && dig && fc_vt_is(dig, FC_DIGGER_VT))
        reread = ((int (*)(uintptr_t, unsigned)) FC_DIGGER_REREAD)(dig, id);
    ((void (*)(uintptr_t, unsigned)) FC_DROP_FOLDER)(cache, id);
    ((void (*)(uintptr_t, unsigned)) FC_DROP_HIER)(cache, LS_CATEGORY_FOLDER);
    drag_log("FCOPY refresh: stick %04x folder %u re-read %d, re-listed\n", media, id, reread);
}

/* Message thread: start copying src_rel (on src_media) into dst_dir (on dst_media). 0 started,
 * <0 refused (a message was shown). */
static int fc_request(unsigned src_media, const char *src_rel, unsigned dst_media,
                      const char *dst_dir, unsigned dst_id, const char *name) {
    if (__atomic_load_n(&g_fc.state, __ATOMIC_ACQUIRE) != FC_IDLE || cp_busy()) {
        az_banner_strip("A copy is still running", PL_MSG_READ);
        return -2;
    }
    if (!src_media || !dst_media || src_media == dst_media) {
        az_banner_strip("Paste on the other USB", PL_MSG_READ);
        return -3;
    }
    char sm[128], dm[128];
    if (!media_mount(src_media, sm, sizeof sm) || !media_mount(dst_media, dm, sizeof dm)) {
        az_banner_strip("USB not found", PL_MSG_READ);
        return -4;
    }
    memset(&g_fc, 0, sizeof g_fc);
    snprintf(g_fc.src_mount, sizeof g_fc.src_mount, "%s", sm);
    snprintf(g_fc.dst_mount, sizeof g_fc.dst_mount, "%s", dm);
    snprintf(g_fc.src_rel, sizeof g_fc.src_rel, "%s", src_rel);
    snprintf(g_fc.dst_dir, sizeof g_fc.dst_dir, "%s", dst_dir);
    snprintf(g_fc.name, sizeof g_fc.name, "%s", name);
    g_fc.dst_media = dst_media;
    g_fc.dst_id = dst_id;
    __atomic_store_n(&g_fc.state, FC_RUNNING, __ATOMIC_RELEASE);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int e = az_thread_create(&t, &at, fc_worker, NULL);
    pthread_attr_destroy(&at);
    if (e) {
        __atomic_store_n(&g_fc.state, FC_IDLE, __ATOMIC_RELEASE);
        az_banner_strip("Copy failed", PL_MSG_READ);
        return -5;
    }
    drag_log("FCOPY start \"%s\" %s -> %s/%s\n", src_rel, sm, dm, dst_dir);
    az_banner_strip("Copying...", 2000);
    return 0;
}

/* Message thread, every tick: progress, then the result once. */
static void fc_tick(void) {
    int st = __atomic_load_n(&g_fc.state, __ATOMIC_ACQUIRE);
    if (st == FC_RUNNING) {
        static int shown = -1;
        static long long at;
        int k = __atomic_load_n(&g_fc.done_n, __ATOMIC_ACQUIRE),
            n = __atomic_load_n(&g_fc.total_n, __ATOMIC_ACQUIRE);
        if (n > 1 && k != shown && drag_now_ms() - at > 800) {
            char b[96];
            snprintf(b, sizeof b, "Copying %d of %d", k + 1 > n ? n : k + 1, n);
            az_banner_strip(b, 1500);
            shown = k;
            at = drag_now_ms();
        }
        return;
    }
    if (st != FC_DONE)
        return;
    int rc = g_fc.rc;
    char b[200];
    if (rc)
        snprintf(b, sizeof b, "%s", fc_why(rc));
    else if (g_fc.skipped)
        snprintf(b, sizeof b, "Already on the other USB: %s", g_fc.name);
    else {
        const char *base = strrchr(g_fc.landed, '/');
        snprintf(b, sizeof b, "Copied %s", pl_name_short(base ? base + 1 : g_fc.landed));
    }
    az_banner_strip(b, PL_MSG_READ);
    if (g_fc.done_n > 0) { /* something landed: have that stick re-list the folder */
        __atomic_store_n(&g_fcr_id, g_fc.dst_id, __ATOMIC_RELEASE);
        __atomic_store_n(&g_fcr_media, g_fc.dst_media, __ATOMIC_RELEASE);
        plrenew_kick();
    }
    drag_log("FCOPY done rc=%d (%d of %d files) %s\n", rc, g_fc.done_n, g_fc.total_n, g_fc.landed);
    __atomic_store_n(&g_fc.state, FC_IDLE, __ATOMIC_RELEASE);
}

/* A stick was mounted: if a copy onto it was interrupted, remove that copy's unfinished
 * "*.azcp" files (the complete ones stay) and the marker. Any thread; cheap without a marker. */
struct fc_clean {
    const char *mount;
    int n;
};
static void fc_clean_dir(struct fc_clean *c, const char *rel, int depth) {
    if (depth > FC_MAX_DEPTH || c->n > FC_MAX_ENTRIES)
        return;
    char p[FC_PATH_MAX + 160];
    if (fc_abs(p, sizeof p, c->mount, rel))
        return;
    DIR *d = opendir(p);
    if (!d)
        return;
    struct dirent *e;
    char (*names)[FC_NAME_MAX + 1] = malloc(64 * sizeof *names); /* heap: this recurses */
    int nd = 0;
    if (!names) {
        closedir(d);
        return;
    }
    while ((e = readdir(d)) && c->n++ <= FC_MAX_ENTRIES) {
        if (!fc_name_ok(e->d_name))
            continue;
        size_t L = strlen(e->d_name);
        char f[FC_PATH_MAX + 160];
        if (L >= 5 && !strcmp(e->d_name + L - 5, ".azcp")) {
            if ((size_t) snprintf(f, sizeof f, "%s/%s", p, e->d_name) < sizeof f)
                unlink(f);
            continue;
        }
        struct stat st;
        if ((size_t) snprintf(f, sizeof f, "%s/%s", p, e->d_name) < sizeof f && lstat(f, &st) == 0 &&
            S_ISDIR(st.st_mode) && nd < 64)
            snprintf(names[nd++], sizeof names[0], "%s", e->d_name);
    }
    closedir(d);
    for (int i = 0; i < nd; i++) {
        char r[FC_PATH_MAX];
        if (!fc_join(r, sizeof r, rel, names[i]))
            fc_clean_dir(c, r, depth + 1);
    }
    free(names);
}
static void fc_mount_cleanup_now(const char *mount) {
    char marker[200], rel[FC_PATH_MAX];
    snprintf(marker, sizeof marker, "%s" FC_MARKER, mount);
    FILE *m = fopen(marker, "re");
    if (!m)
        return;
    int ok = fgets(rel, sizeof rel, m) != NULL;
    fclose(m);
    rel[strcspn(rel, "\n")] = 0;
    /* the marker names one path the copy made; only a valid relative path is followed */
    int valid = ok && rel[0] && rel[0] != '/';
    for (char *c = rel, *s = rel; valid && *s; s = c) {
        c = strchr(s, '/');
        char part[FC_NAME_MAX + 1];
        size_t L = c ? (size_t) (c - s) : strlen(s);
        if (L > FC_NAME_MAX) {
            valid = 0;
            break;
        }
        memcpy(part, s, L);
        part[L] = 0;
        valid = fc_name_ok(part);
        if (!c)
            break;
        c++;
    }
    if (valid) {
        struct fc_clean cl = {mount, 0};
        struct stat st;
        char p[FC_PATH_MAX + 160];
        if (!fc_abs(p, sizeof p, mount, rel) && lstat(p, &st) == 0) {
            if (S_ISDIR(st.st_mode))
                fc_clean_dir(&cl, rel, 0);
            else {
                char t[FC_PATH_MAX + 200];
                if ((size_t) snprintf(t, sizeof t, "%s.azcp", p) < sizeof t)
                    unlink(t);
            }
        } else { /* a single file that never landed: only its .azcp */
            char t[FC_PATH_MAX + 200];
            if (!fc_abs(p, sizeof p, mount, rel) && (size_t) snprintf(t, sizeof t, "%s.azcp", p) < sizeof t)
                unlink(t);
        }
    }
    unlink(marker);
    drag_log("FCOPY cleanup at mount %s (%s)\n", mount, valid ? rel : "marker not a valid path");
}
static void *fc_cleanup_thread(void *arg) {
    struct sched_param sp = {0};
    pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp);
    fc_mount_cleanup_now((const char *) arg);
    free(arg);
    return NULL;
}
/* Called when a stick's library comes up (any thread): one stat when there is no marker,
 * else the cleanup on its own idle thread. */
static void fc_mount_cleanup(const char *mount) {
    char marker[200];
    struct stat st;
    if ((size_t) snprintf(marker, sizeof marker, "%s" FC_MARKER, mount) >= sizeof marker ||
        stat(marker, &st) != 0)
        return;
    char *m = strdup(mount);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (!m || az_thread_create(&t, &at, fc_cleanup_thread, m))
        free(m);
    pthread_attr_destroy(&at);
}
