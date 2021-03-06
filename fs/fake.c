#include <stdarg.h>
#include <limits.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <gdbm.h>

#include "debug.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "fs/fd.h"

// TODO document database

struct ish_stat {
    dword_t mode;
    dword_t uid;
    dword_t gid;
    dword_t rdev;
};

static void gdbm_fatal(const char *thingy) {
    printk("fatal gdbm error: %s\n", thingy);
}

static void db_recovery_error(void *data, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintk(fmt, args);
    printk("\n");
    va_end(args);
}
// check the error, do what needs to be done, return 1 if you should retry
static int check_db_err(GDBM_FILE db) {
    if (gdbm_needs_recovery(db)) {
        printk("recovering database\n");
        gdbm_recovery recovery = {
            .errfun = db_recovery_error, .data = NULL,
        };
        int err = gdbm_recover(db, &recovery, GDBM_RCVR_BACKUP);
        printk("recovery finished, %d lost keys, %d lost buckets, backed up to %s\n",
                recovery.failed_keys, recovery.failed_buckets, recovery.backup_name);
        if (err != 0) {
            printk("recovery failed\n");
            abort(); // TODO something less mean
        }
        return 1;
    }
    if (gdbm_last_errno(db) == 0 || gdbm_last_errno(db) == GDBM_ITEM_NOT_FOUND)
        return 0;
    printk("gdbm error: %s\n", gdbm_db_strerror(db));
    abort();
}

static void lock_db(struct mount *mount) {
    int fd = gdbm_fdesc(mount->db);
    while (true) {
        int err = flock(fd, LOCK_EX);
        if (err == 0)
            break;
        if (err < 0 && errno != EINTR)
            DIE("could not lock database");
    }
}
static void unlock_db(struct mount *mount) {
    int fd = gdbm_fdesc(mount->db);
    int err = flock(fd, LOCK_UN);
    if (err < 0)
        DIE("could not unlock database");
}

static datum make_datum(char *data, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = vsprintf(data, format, args);
    va_end(args);
    return (datum) {.dptr = data, .dsize = n};
}

static datum read_meta(struct mount *mount, datum key) {
    datum value;
    do {
        value = gdbm_fetch(mount->db, key);
    } while (value.dptr == NULL && check_db_err(mount->db));
    return value;
}

static void write_meta(struct mount *mount, datum key, datum data) {
    int err;
    do {
        err = gdbm_store(mount->db, key, data, GDBM_REPLACE);
    } while (err == -1 && check_db_err(mount->db));
}

static void delete_meta(struct mount *mount, datum key) {
    int err;
    do {
        err = gdbm_delete(mount->db, key);
    } while (err == -1 && check_db_err(mount->db));
}

static ino_t inode_for_path(struct mount *mount, const char *path) {
    struct stat stat;
    if (fstatat(mount->root_fd, fix_path(path), &stat, AT_SYMLINK_NOFOLLOW) < 0)
        // interestingly, both linux and darwin reserve inode number 0. linux
        // uses it for an error return and darwin uses it to mark deleted
        // directory entries (and maybe also for error returns, I don't know).
        return 0;
    return stat.st_ino;
}

static ino_t write_path(struct mount *mount, const char *path) {
    ino_t inode = inode_for_path(mount, path);
    if (inode != 0) {
        char keydata[MAX_PATH+strlen("inode")+1];
        char valuedata[30];
        write_meta(mount,
                make_datum(keydata, "inode %s", path),
                make_datum(valuedata, "%lu", inode));
    }
    return inode;
}

static void delete_path(struct mount *mount, const char *path) {
    char keydata[MAX_PATH+strlen("inode")+1];
    delete_meta(mount, make_datum(keydata, "inode %s", path));
}

static datum stat_key(char *data, struct mount *mount, const char *path) {
    // record the path-inode correspondence, in case there was a crash before
    // this could be recorded when the file was created
    ino_t inode = write_path(mount, path);
    if (inode == 0)
        return (datum) {};
    return make_datum(data, "stat %lu", inode);
}

static bool read_stat(struct mount *mount, const char *path, struct ish_stat *stat) {
    char keydata[30];
    datum d = read_meta(mount, stat_key(keydata, mount, path));
    if (d.dptr == NULL)
        return false;
    assert(d.dsize == sizeof(struct ish_stat));
    if (stat != NULL)
        *stat = *(struct ish_stat *) d.dptr;
    free(d.dptr);
    return true;
}

static void write_stat(struct mount *mount, const char *path, struct ish_stat *stat) {
    char keydata[30];
    datum key = stat_key(keydata, mount, path);
    assert(key.dptr != NULL);
    datum data;
    data.dptr = (void *) stat;
    data.dsize = sizeof(struct ish_stat);
    write_meta(mount, key, data);
}

static struct fd *fakefs_open(struct mount *mount, const char *path, int flags, int mode) {
    struct fd *fd = realfs.open(mount, path, flags, 0666);
    if (IS_ERR(fd))
        return fd;
    if (flags & O_CREAT_) {
        if (!read_stat(mount, path, NULL)) {
            struct ish_stat ishstat;
            ishstat.mode = mode | S_IFREG;
            ishstat.uid = current->uid;
            ishstat.gid = current->gid;
            ishstat.rdev = 0;
            lock_db(mount);
            write_stat(mount, path, &ishstat);
            unlock_db(mount);
        }
    }
    return fd;
}

static int fakefs_link(struct mount *mount, const char *src, const char *dst) {
    lock_db(mount);
    int err = realfs.link(mount, src, dst);
    if (err < 0) {
        unlock_db(mount);
        return err;
    }
    write_path(mount, dst);
    unlock_db(mount);
    return 0;
}

static int fakefs_unlink(struct mount *mount, const char *path) {
    // find out if this is the last link
    bool gone = false;
    int fd = openat(mount->root_fd, fix_path(path), O_RDONLY);
    struct stat stat;
    if (fd >= 0 && fstat(fd, &stat) >= 0 && stat.st_nlink == 1)
        gone = true;
    if (fd >= 0)
        close(fd);

    lock_db(mount);
    char keydata[30];
    datum key = stat_key(keydata, mount, path);
    int err = realfs.unlink(mount, path);
    if (err < 0) {
        unlock_db(mount);
        return err;
    }
    delete_path(mount, path);
    if (gone)
        delete_meta(mount, key);
    unlock_db(mount);
    return 0;
}

static int fakefs_rmdir(struct mount *mount, const char *path) {
    lock_db(mount);
    char keydata[30];
    datum key = stat_key(keydata, mount, path);
    int err = realfs.rmdir(mount, path);
    if (err < 0) {
        unlock_db(mount);
        return err;
    }
    delete_path(mount, path);
    delete_meta(mount, key);
    unlock_db(mount);
    return 0;
}

static int fakefs_rename(struct mount *mount, const char *src, const char *dst) {
    lock_db(mount);
    // get the inode of the dst path
    char keydata[30];
    datum key = stat_key(keydata, mount, dst);
    ino_t old_dst_inode = inode_for_path(mount, dst);

    int err = realfs.rename(mount, src, dst);
    if (err < 0) {
        unlock_db(mount);
        return err;
    }
    write_path(mount, dst);
    delete_path(mount, src);
    // if this rename clobbered a file at the dst path, the metadata for that
    // file needs to be deleted
    if (old_dst_inode != 0 && old_dst_inode != inode_for_path(mount, dst))
        delete_meta(mount, key);
    unlock_db(mount);
    return 0;
}

static int fakefs_symlink(struct mount *mount, const char *target, const char *link) {
    lock_db(mount);
    // create a file containing the target
    int fd = openat(mount->root_fd, fix_path(link), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        unlock_db(mount);
        return errno_map();
    }
    ssize_t res = write(fd, target, strlen(target));
    close(fd);
    if (res < 0) {
        int saved_errno = errno;
        unlinkat(mount->root_fd, fix_path(link), 0);
        unlock_db(mount);
        errno = saved_errno;
        return errno_map();
    }

    // customize the stat info so it looks like a link
    struct ish_stat ishstat;
    ishstat.mode = S_IFLNK | 0777; // symlinks always have full permissions
    ishstat.uid = current->uid;
    ishstat.gid = current->gid;
    ishstat.rdev = 0;
    write_stat(mount, link, &ishstat);
    unlock_db(mount);
    return 0;
}

static int fakefs_stat(struct mount *mount, const char *path, struct statbuf *fake_stat, bool follow_links) {
    lock_db(mount);
    struct ish_stat ishstat;
    if (!read_stat(mount, path, &ishstat)) {
        unlock_db(mount);
        return _ENOENT;
    }
    int err = realfs.stat(mount, path, fake_stat, follow_links);
    unlock_db(mount);
    if (err < 0)
        return err;
    fake_stat->mode = ishstat.mode;
    fake_stat->uid = ishstat.uid;
    fake_stat->gid = ishstat.gid;
    fake_stat->rdev = ishstat.rdev;
    return 0;
}

static int fakefs_fstat(struct fd *fd, struct statbuf *fake_stat) {
    // this is truly sad, but there is no alternative
    char path[MAX_PATH];
    int err = fd->mount->fs->getpath(fd, path);
    if (err < 0)
        return err;
    return fakefs_stat(fd->mount, path, fake_stat, false);
}

static int fakefs_setattr(struct mount *mount, const char *path, struct attr attr) {
    lock_db(mount);
    struct ish_stat ishstat;
    if (!read_stat(mount, path, &ishstat)) {
        unlock_db(mount);
        return _ENOENT;
    }
    switch (attr.type) {
        case attr_uid:
            ishstat.uid = attr.uid;
            break;
        case attr_gid:
            ishstat.gid = attr.gid;
            break;
        case attr_mode:
            ishstat.mode = (ishstat.mode & S_IFMT) | (attr.mode & ~S_IFMT);
            break;
        case attr_size:
            unlock_db(mount);
            return realfs_truncate(mount, path, attr.size);
    }
    write_stat(mount, path, &ishstat);
    unlock_db(mount);
    return 0;
}

static int fakefs_fsetattr(struct fd *fd, struct attr attr) {
    char path[MAX_PATH];
    int err = fd->mount->fs->getpath(fd, path);
    if (err < 0)
        return err;
    return fakefs_setattr(fd->mount, path, attr);
}

static int fakefs_mkdir(struct mount *mount, const char *path, mode_t_ mode) {
    lock_db(mount);
    int err = realfs.mkdir(mount, path, 0777);
    if (err < 0) {
        unlock_db(mount);
        return err;
    }
    struct ish_stat ishstat;
    ishstat.mode = mode | S_IFDIR;
    ishstat.uid = current->uid;
    ishstat.gid = current->gid;
    ishstat.rdev = 0;
    write_stat(mount, path, &ishstat);
    unlock_db(mount);
    return 0;
}

static ssize_t file_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    // broken symlinks can't be included in an iOS app or else Xcode craps out
    int fd = openat(mount->root_fd, fix_path(path), O_RDONLY);
    if (fd < 0)
        return errno_map();
    int err = read(fd, buf, bufsize);
    close(fd);
    if (err < 0)
        return errno_map();
    return err;
}

static ssize_t fakefs_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    lock_db(mount);
    struct ish_stat ishstat;
    if (!read_stat(mount, path, &ishstat)) {
        unlock_db(mount);
        return _ENOENT;
    }
    if (!S_ISLNK(ishstat.mode)) {
        unlock_db(mount);
        return _EINVAL;
    }

    ssize_t err = realfs.readlink(mount, path, buf, bufsize);
    if (err == _EINVAL)
        err = file_readlink(mount, path, buf, bufsize);
    unlock_db(mount);
    return err;
}

int fakefs_rebuild(struct mount *mount, const char *db_path);

static int fakefs_mount(struct mount *mount) {
    char db_path[PATH_MAX];
    strcpy(db_path, mount->source);
    char *basename = strrchr(db_path, '/') + 1;
    assert(strcmp(basename, "data") == 0);
    strcpy(basename, "meta.db");
    mount->db = gdbm_open(db_path, 0, GDBM_NOLOCK | GDBM_WRITER | GDBM_SYNC, 0, gdbm_fatal);
    if (mount->db == NULL) {
        printk("gdbm error: %s\n", gdbm_strerror(gdbm_errno));
        return _EINVAL;
    }

    // do this now so fakefs_rebuild can use mount->root_fd
    int err = realfs.mount(mount);
    if (err < 0)
        return err;

    // after the filesystem is compressed, transmitted, and uncompressed, the
    // inode numbers will be different. to detect this, the inode of the
    // database file is stored inside the database and compared with the actual
    // database file inode, and if they're different we rebuild the database.
    struct stat stat;
    if (fstat(gdbm_fdesc(mount->db), &stat) < 0) DIE("fstat database");
    datum key = {.dptr = "db inode", .dsize = strlen("db inode")};
    datum value = gdbm_fetch(mount->db, key);
    if (value.dptr != NULL) {
        if (atol(value.dptr) != stat.st_ino) {
            int err = fakefs_rebuild(mount, db_path);
            if (err < 0) {
                close(mount->root_fd);
                return err;
            }
        }
        free(value.dptr);
    }

    char keydata[30];
    value = make_datum(keydata, "%lu", (unsigned long) stat.st_ino);
    value.dsize++; // make sure to null terminate
    gdbm_store(mount->db, key, value, GDBM_REPLACE);

    return 0;
}

static int fakefs_umount(struct mount *mount) {
    if (mount->data)
        gdbm_close(mount->data);
    /* return realfs.umount(mount); */
    return 0;
}

const struct fs_ops fakefs = {
    .mount = fakefs_mount,
    .umount = fakefs_umount,
    .statfs = realfs_statfs,
    .open = fakefs_open,
    .readlink = fakefs_readlink,
    .link = fakefs_link,
    .unlink = fakefs_unlink,
    .rename = fakefs_rename,
    .symlink = fakefs_symlink,

    .stat = fakefs_stat,
    .fstat = fakefs_fstat,
    .flock = realfs_flock,
    .setattr = fakefs_setattr,
    .fsetattr = fakefs_fsetattr,
    .getpath = realfs_getpath,
    .utime = realfs_utime,

    .mkdir = fakefs_mkdir,
    .rmdir = fakefs_rmdir,
};
