/*
 * hangfs.c — minimal FUSE filesystem emulating a stalled (hanging) disk,
 * used by the bootroll Milestone-8 acceptance item 6 (root-side
 * stall-warning test, see docs/M8_ACCEPTANCE.md §3.2 / docs/LESSONS.md #20).
 *
 * Behaviour:
 *   - Serves a single 64 MiB file "/blank.img" whose contents are all zeros.
 *   - A monotonic gate starts at the FIRST open of any file: reads succeed
 *     normally for GATE_SECONDS (long enough for `losetup -f --show` to
 *     attach the image and for the kernel to finish probing), after which
 *     every read blocks forever — emulating a device whose reads hang.
 *
 * Why it is built this way (LESSONS #20):
 *   - Mounted with direct_io so reads bypass the page cache and reach the
 *     daemon every time (cached reads would never hang);
 *   - The gate must exceed the losetup probe sequence (~seconds); anything
 *     below ~30 s wedges the attach itself into D state;
 *   - Hung reads never return, so FUSE requests pile up. Stop the daemon
 *     with SIGKILL (kill -9 <pid>, never a wide `pkill -f` — LESSONS #19).
 *     The kernel wakes the stuck readers with EIO, which unwinds loop
 *     device users stuck in D state. Recovery order:
 *       kill -9 <hangfs pid>  →  losetup -d /dev/loopN  →  fusermount3 -uz <mnt>
 *
 * Build:
 *   cc -Wall -Wextra -o hangfs hangfs.c $(pkg-config fuse3 --cflags --libs)
 *
 * Usage:
 *   ./hangfs [-f] <mountpoint>        (-f keeps it in the foreground)
 *
 * The -o direct_io mount option is inserted programmatically, so callers
 * do not have to remember it.
 */

#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define IMG_PATH    "/blank.img"
#define IMG_SIZE    (64ULL * 1024ULL * 1024ULL) /* 64 MiB, all zeros */
#define GATE_SECS   30.0

/* Monotonic timestamp (seconds) of the first open; < 0 means "no open yet". */
static double g_open_epoch = -1.0;

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int hf_getattr(const char* path, struct stat* st,
                      struct fuse_file_info* fi)
{
    (void)fi;
    if (strcmp(path, "/") != 0 && strcmp(path, IMG_PATH) != 0) {
        return -ENOENT;
    }
    memset(st, 0, sizeof *st);
    st->st_uid = getuid();
    st->st_gid = getgid();
    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        st->st_ino = 1;
    } else {
        st->st_mode = S_IFREG | 0444; /* read-only, like a probe device */
        st->st_nlink = 1;
        st->st_size = (off_t)IMG_SIZE;
        st->st_blksize = 4096;
        st->st_blocks = (blkcnt_t)(IMG_SIZE / 512);
        st->st_ino = 2;
    }
    return 0;
}

static int hf_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                      off_t off, struct fuse_file_info* fi,
                      enum fuse_readdir_flags flags)
{
    (void)off;
    (void)fi;
    (void)flags;
    if (strcmp(path, "/") != 0) {
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0, (enum fuse_fill_dir_flags)0);
    filler(buf, "..", NULL, 0, (enum fuse_fill_dir_flags)0);
    filler(buf, "blank.img", NULL, 0, (enum fuse_fill_dir_flags)0);
    return 0;
}

static int hf_open(const char* path, struct fuse_file_info* fi)
{
    if (strcmp(path, IMG_PATH) != 0) {
        return -ENOENT;
    }
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        return -EACCES;
    }
    if (g_open_epoch < 0.0) {
        g_open_epoch = mono_now();
        fprintf(stderr, "hangfs: first open — %gs gate started\n", GATE_SECS);
    }
    fprintf(stderr, "hangfs: open (elapsed %.1fs)\n",
            mono_now() - g_open_epoch);
    return 0;
}

static int hf_read(const char* path, char* buf, size_t size, off_t off,
                   struct fuse_file_info* fi)
{
    (void)fi;
    if (strcmp(path, IMG_PATH) != 0) {
        return -ENOENT;
    }
    if (off >= (off_t)IMG_SIZE) {
        return 0;
    }
    if ((off_t)size > (off_t)IMG_SIZE - off) {
        size = (size_t)((off_t)IMG_SIZE - off);
    }
    if (g_open_epoch >= 0.0 && mono_now() - g_open_epoch >= GATE_SECS) {
        /* Gate closed: emulate the stalled device. This request never
         * returns; the daemon must be stopped with SIGKILL afterwards. */
        fprintf(stderr, "hangfs: read HANGS (off=%lld size=%zu)\n",
                (long long)off, size);
        fflush(stderr);
        for (;;) {
            sleep(60);
        }
    }
    memset(buf, 0, size);
    fprintf(stderr, "hangfs: read ok (off=%lld size=%zu)\n",
            (long long)off, size);
    return (int)size;
}

static void* hf_init(struct fuse_conn_info* conn, struct fuse_config* cfg)
{
    (void)conn;
    /* direct_io: every read reaches the daemon (LESSONS #20, point 1).
     * In libfuse3 the mount option moved into fuse_config. */
    cfg->direct_io = 1;
    fprintf(stderr, "hangfs: direct_io enabled via fuse_config\n");
    return NULL;
}

static const struct fuse_operations g_ops = {
    .getattr = hf_getattr,
    .readdir = hf_readdir,
    .open = hf_open,
    .read = hf_read,
    .init = hf_init,
};

int main(int argc, char* argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    return fuse_main(args.argc, args.argv, &g_ops, NULL);
}
