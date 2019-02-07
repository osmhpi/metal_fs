#define FUSE_USE_VERSION 31

#define _XOPEN_SOURCE 700

#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <libgen.h>
#include <dirent.h>

#include <pthread.h>

extern "C" {
#include "../common/known_operators.h"
#include <fuse.h>
#include "../../metal/metal.h"
#include "../../metal/inode.h"
};

#include "server.hpp"

static char agent_filepath[255];

static const char *operators_dir = "operators";
static const char *files_dir = "files";
static const char *socket_alias = ".hello";
static char socket_filename[255];


static int chown_callback(const char *path, uid_t uid, gid_t gid) {
    return mtl_chown(path + 6, uid, gid);
}

static int getattr_callback(const char *path, struct stat *stbuf) {
    memset(stbuf, 0, sizeof(struct stat));

    int res;
    char test_filename[FILENAME_MAX];

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    snprintf(test_filename, FILENAME_MAX, "/%s", operators_dir);
    if (strcmp(path, test_filename) == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    snprintf(test_filename, FILENAME_MAX, "/%s", files_dir);
    if (strcmp(path, test_filename) == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    snprintf(test_filename, FILENAME_MAX, "/%s/", files_dir);
    if (strncmp(path, test_filename, strlen(test_filename)) == 0) {
          mtl_inode inode;

          res = mtl_get_inode(path + 6, &inode);
          if (res != MTL_SUCCESS) {
            return -res;
          }

          (void)stbuf->st_dev; // device ID? can we put something meaningful here?
          (void)stbuf->st_ino; // TODO: inode-ID
          (void)stbuf->st_mode; // set according to filetype below
          (void)stbuf->st_nlink; // number of hard links to file. since we don't support hardlinks as of now, this will always be 0.
          stbuf->st_uid = inode.user; // user-ID of owner
          stbuf->st_gid = inode.group; // group-ID of owner
          (void)stbuf->st_rdev; // unused, since this field is meant for special files which we do not have in our FS
          stbuf->st_size = inode.length; // length of referenced file in byte
          (void)stbuf->st_blksize; // our blocksize. 4k?
          (void)stbuf->st_blocks; // number of 512B blocks belonging to file. TODO: needs to be set in inode whenever we write an extent
          stbuf->st_atime = inode.accessed; // time of last read or write
          stbuf->st_mtime = inode.modified; // time of last write
          stbuf->st_ctime = inode.created; // time of last change to either content or inode-data

          stbuf->st_mode =
            inode.type == MTL_FILE
                ? S_IFREG | 0755
                : S_IFDIR | 0755;
          stbuf->st_nlink = 2;
          return 0;
    }

    snprintf(test_filename, FILENAME_MAX, "/%s", socket_alias);
    if (strcmp(path, test_filename) == 0) {
        stbuf->st_mode = S_IFLNK | 0777;
        stbuf->st_nlink = 1;
        stbuf->st_size = strlen(socket_filename);
        return 0;
    }

    for (size_t i = 0; i < sizeof(known_operators) / sizeof(known_operators[0]); ++i) {
        snprintf(test_filename, FILENAME_MAX, "/%s/%s", operators_dir, known_operators[i]->name);
        if (strcmp(path, test_filename) != 0) {
            continue;
        }

        res = lstat(agent_filepath, stbuf);
        if (res == -1)
            return -errno;

        return 0;
    }

    return -ENOENT;
}

static int readdir_callback(const char *path, void *buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info *fi) {
    (void) offset;
    (void) fi;

    char test_filename[FILENAME_MAX];

    if (strcmp(path, "/") == 0) {
        filler(buf, ".", NULL, 0);
        filler(buf, "..", NULL, 0);
        filler(buf, socket_alias, NULL, 0);
        filler(buf, operators_dir, NULL, 0);
        filler(buf, files_dir, NULL, 0);
        return 0;
    }

    snprintf(test_filename, FILENAME_MAX, "/%s", operators_dir);
    if (strcmp(path, test_filename) == 0) {
        filler(buf, ".", NULL, 0);
        filler(buf, "..", NULL, 0);
        for (size_t i = 0; i < sizeof(known_operators) / sizeof(known_operators[0]); ++i) {
            filler(buf, known_operators[i]->name, NULL, 0);
        }
        return 0;
    }

    snprintf(test_filename, FILENAME_MAX, "/%s/", files_dir);
    if ((strlen(path) == 6 && strncmp(path, test_filename, 6) == 0) ||
         strncmp(path, test_filename, strlen(test_filename)) == 0) {
        mtl_dir *dir;

        // +6, because path = "/files/<filename>", but we only want "/filename"
        int res;
        if (strlen(path + 6) == 0) {
            res = mtl_opendir("/", &dir);
        } else {
            res = mtl_opendir(path + 6, &dir);
        }

        if (res != MTL_SUCCESS) {
            return -res;
        }

        char current_filename[FILENAME_MAX];
        int readdir_status;
        while ((readdir_status = mtl_readdir(dir, current_filename, sizeof(current_filename))) != MTL_COMPLETE) {
            filler(buf, current_filename, NULL, 0);
        }
    }

    return 0;
}

static int create_callback(const char *path, mode_t mode, struct fuse_file_info *fi) {
    int res;

    char test_filename[FILENAME_MAX];
    snprintf(test_filename, FILENAME_MAX, "/%s", files_dir);
    if (strncmp(path, test_filename, strlen(test_filename)) == 0) {
        uint64_t inode_id;
        res = mtl_create(path + 6, &inode_id);
        if (res != MTL_SUCCESS)
            return -res;

        fi->fh = inode_id;

        return 0;
    }

    fprintf(stderr, "ENOSYS in create_callback(\"%s\", %o, %p)", path, mode, fi);
    return -ENOSYS;
}

static int readlink_callback(const char *path, char *buf, size_t size) {
    if (strncmp(path, "/", 1) == 0) {  // probably nonsense
        if (strcmp(path+1, socket_alias) == 0) {
            strncpy(buf, socket_filename, size);
            return 0;
        }
    }

    return -ENOENT;
}

static int open_callback(const char *path, struct fuse_file_info *fi) {

    int res;

    char test_filename[FILENAME_MAX];
    snprintf(test_filename, FILENAME_MAX, "/%s/", files_dir);
    if (strncmp(path, test_filename, strlen(test_filename)) == 0) {
        uint64_t inode_id;
        res = mtl_open(path + 6, &inode_id);

        if (res != MTL_SUCCESS)
            return -res;

        fi->fh = inode_id;

        return 0;
    }

    fprintf(stderr, "ENOSYS in open_callback(\"%s\", %p)", path, fi);
    return -ENOSYS;
}

static int read_callback(const char *path, char *buf, size_t size, off_t offset,
    struct fuse_file_info *fi) {

    if (fi->fh != 0) {
        return mtl_read(fi->fh, buf, size, offset);
    }

    char test_filename[FILENAME_MAX];

    snprintf(test_filename, FILENAME_MAX, "/%s", files_dir);
    if (strncmp(path, test_filename, strlen(test_filename)) == 0) {
        int res;

        // TODO: It would be nice if this would work within a single transaction
        uint64_t inode_id;
        res = mtl_open(path + 6, &inode_id);
        if (res != MTL_SUCCESS)
            return -res;

        return mtl_read(inode_id, buf, size, offset);
    }

    for (size_t i = 0; i < sizeof(known_operators) / sizeof(known_operators[0]); ++i) {
        snprintf(test_filename, FILENAME_MAX, "/%s/%s", operators_dir, known_operators[i]->name);
        if (strcmp(path, test_filename) != 0) {
            continue;
        }

        int fd;
        int res;

        // if (fi == NULL || true)
        fd = open(agent_filepath, O_RDONLY);
        // else
        //     fd = fi->fh;

        if (fd == -1)
            return -errno;

        res = pread(fd, buf, size, offset);
        if (res == -1)
            res = -errno;

        if(fi == NULL || true)
            close(fd);
        return res;
    }

    fprintf(stderr, "ENOSYS in read_callback(\"%s\", %p, %d, %d, %p)", path, buf, size, offset, fi);
    return -ENOSYS;
}

static int release_callback(const char *path, struct fuse_file_info *fi)
{
    return 0;
}

static int truncate_callback(const char *path, off_t size)
{
    // struct fuse_file_info* is not available in truncate :/
    int res;

    char test_filename[FILENAME_MAX];
    snprintf(test_filename, FILENAME_MAX, "/%s", files_dir);
    if (strncmp(path, test_filename, strlen(test_filename)) == 0) {
        uint64_t inode_id;
        res = mtl_open(path + 6, &inode_id);

        if (res != MTL_SUCCESS)
            return -res;

        res = mtl_truncate(inode_id, size);

        if (res != MTL_SUCCESS)
            return -res;

        return 0;
    }

    fprintf(stderr, "ENOSYS in truncate_callback(\"%s\", %d)", path, size);
    return -ENOSYS;
}

static int write_callback(const char *path, const char *buf, size_t size,
        off_t offset, struct fuse_file_info *fi)
{
    if (fi->fh != 0) {
        mtl_write(fi->fh, buf, size, offset);

        // TODO: Return the actual length that was written (to be returned from mtl_write)
        return size;
    }

    fprintf(stderr, "ENOSYS in write_callback(\"%s\", %p, %d, %d, %p)", path, buf, size, offset, fi);
    return -ENOSYS;
}

static int unlink_callback(const char *path) {

    int res;

    char test_filename[FILENAME_MAX];
    snprintf(test_filename, FILENAME_MAX, "/%s/", files_dir);
    if (strncmp(path, test_filename, strlen(test_filename)) == 0) {

        res = mtl_unlink(path + 6);

        if (res != MTL_SUCCESS)
            return -res;

        return 0;
    }

    fprintf(stderr, "ENOSYS in unlink_callback(\"%s\")", path);
    return -ENOSYS;
}

static int mkdir_callback(const char *path, mode_t mode)
{
    int res;

    char test_filename[FILENAME_MAX];
    snprintf(test_filename, FILENAME_MAX, "/%s/", files_dir);
    if (strncmp(path, test_filename, strlen(test_filename)) == 0) {

        res = mtl_mkdir(path + 6);

        if (res != MTL_SUCCESS)
            return -res;

        return 0;
    }

    fprintf(stderr, "ENOSYS in mkdir_callback(\"%s\", %o)", path, mode);
    return -ENOSYS;
}


static int rmdir_callback(const char *path)
{
    int res;

    char test_filename[FILENAME_MAX];
    snprintf(test_filename, FILENAME_MAX, "/%s/", files_dir);
    if (strncmp(path, test_filename, strlen(test_filename)) == 0) {

        res = mtl_rmdir(path + 6);

        if (res != MTL_SUCCESS)
            return -res;

        return 0;
    }

    fprintf(stderr, "ENOSYS in rmdir_callback(\"%s\")", path);
    return -ENOSYS;
}

static int rename_callback(const char * from_path, const char * to_path) {
    int res;

    char test_filename[FILENAME_MAX];
    snprintf(test_filename, FILENAME_MAX, "/%s/", files_dir);
    if (strncmp(from_path, test_filename, strlen(test_filename)) == 0 &&
        strncmp(to_path,   test_filename, strlen(test_filename)) == 0) {

        res = mtl_rename(from_path + 6, to_path + 6);

        if (res != MTL_SUCCESS)
            return -res;

        return 0;
    }

    fprintf(stderr, "ENOSYS in rename_callback(\"%s\", \"%s\")", from_path, to_path);
    return -ENOSYS;
}

// debug!
static int flush_callback(const char *path, struct fuse_file_info *fi) {
    return 0;
}

static int mknod_callback_d(const char * path, mode_t mode, dev_t dev) {
    fprintf(stderr, "ENOSYS in mknod_callback(\"%s\", %o, %x)", path, mode, dev);
    return -ENOSYS;
}

static int symlink_callback_d(const char * path0, const char * path1) {
    fprintf(stderr, "ENOSYS in symlink_callback(\"%s\", \"%s\")", path0, path1);
    return -ENOSYS;
}


static int link_callback_d(const char * path0, const char * path1) {
    fprintf(stderr, "ENOSYS in link_callback(\"%s\", \"%s\")", path0, path1);
    return -ENOSYS;
}

static int chmod_callback_d(const char * path, mode_t mode) {
    fprintf(stderr, "ENOSYS in chmod_callback(\"%s\", %o)", path, mode);
    return -ENOSYS;
}

static int statfs_callback_d(const char * path, struct statvfs * svfs) {
    fprintf(stderr, "ENOSYS in statfs_callback(\"%s\", %p)", path, svfs);
    return -ENOSYS;
}

static int fsync_callback_d(const char * path, int arg, struct fuse_file_info * fi) {
    fprintf(stderr, "ENOSYS in fsync_callback(\"%s\", %d, %p)", path, arg, fi);
    return -ENOSYS;
}

constexpr struct fuse_operations fuse_example_operations = []{
    struct fuse_operations ops{};
    ops.create = create_callback; //?
    ops.flush = flush_callback; //?
    ops.getattr = getattr_callback;
    /* ops.access = xmp_access; */
    ops.readlink = readlink_callback;
    ops.readdir = readdir_callback;
    ops.mknod = mknod_callback_d;
    ops.mkdir = mkdir_callback;
    ops.symlink = symlink_callback_d;
    ops.unlink = unlink_callback;
    ops.rmdir = rmdir_callback;
    ops.rename = rename_callback;
    ops.link = link_callback_d;
    ops.chmod = chmod_callback_d;
    ops.chown = chown_callback;
    ops.truncate = truncate_callback;
    ops.open = open_callback;
    ops.read = read_callback;
    ops.write = write_callback;
    ops.statfs = statfs_callback_d;
    ops.release = release_callback;
    ops.fsync = fsync_callback_d;
    return ops;
}();

int main(int argc, char *argv[])
{
    // Set a file name for the server socket
    char temp[L_tmpnam];
    tmpnam(temp);
    strncpy(socket_filename, temp, 255);

    // Determine the path to the operator_agent executable
    strncpy(agent_filepath, argv[0], sizeof(agent_filepath));
    dirname(agent_filepath);
    strncat(agent_filepath, "/operator_agent", sizeof(agent_filepath));

    pthread_t server_thread;
    pthread_create(&server_thread, NULL, start_socket, (void*)socket_filename);

    DIR* dir = opendir("metadata_store");
    if (dir) {
        closedir(dir);
    } else if (ENOENT == errno) {
        mkdir("metadata_store", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    }

    if (mtl_initialize("metadata_store") != MTL_SUCCESS)
        return 1;
    // mtl_pipeline_initialize();

    int retc = fuse_main(argc, argv, &fuse_example_operations, NULL);

    // This de-allocates the action/card, so this must be called every time we exit
    mtl_deinitialize();
    // mtl_pipeline_deinitialize();

    return retc;
}