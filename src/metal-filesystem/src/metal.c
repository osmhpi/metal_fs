#define _XOPEN_SOURCE 700

#include <assert.h>
#include <libgen.h>
#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lmdb.h>

#include <metal-filesystem/extent.h>
#include <metal-filesystem/heap.h>
#include <metal-filesystem/inode.h>
#include <metal-filesystem/metal.h>

#include "meta.h"

typedef struct mtl_dir {
  uint64_t length;
  mtl_directory_entry_head *first;
  mtl_directory_entry_head *next;
} mtl_dir;

typedef struct mtl_context {
  MDB_env *env;
  mtl_storage_metadata metadata;
  mtl_storage_backend *storage;
} mtl_context;

int mtl_initialize(mtl_context **context, const char *metadata_store,
                   mtl_storage_backend *storage) {
  mtl_context *ctx = (mtl_context *)malloc(sizeof(mtl_context));
  ctx->storage = storage;

  int res = ctx->storage->initialize(ctx->storage->context);
  if (res != MTL_SUCCESS) {
    return MTL_ERROR_INVALID_ARGUMENT;
  }

  mdb_env_create(&ctx->env);
  mdb_env_set_maxdbs(ctx->env, 4);  // inodes, extents, heap, meta
  res = mdb_env_open(ctx->env, metadata_store, 0, 0644);

  if (res == MDB_INVALID) {
    return MTL_ERROR_INVALID_ARGUMENT;
  }

  MDB_txn *txn;
  mdb_txn_begin(ctx->env, NULL, 0, &txn);

  mtl_create_root_directory(txn);

  // Query storage metadata
  storage->get_metadata(ctx->storage->context, &ctx->metadata);

  // Create a single extent spanning the entire storage (if necessary)
  mtl_initialize_extents(txn, ctx->metadata.num_blocks);

  // mtl_dump_extents(txn);

  mdb_txn_commit(txn);

  *context = ctx;

  return MTL_SUCCESS;
}

int mtl_deinitialize(mtl_context *context) {
  context->storage->deinitialize(context->storage->context);

  mdb_env_close(context->env);

  free(context);

  return MTL_SUCCESS;
}

int mtl_resolve_inode(MDB_txn *txn, const char *path, uint64_t *inode_id) {
  int res;
  char *dirc, *basec;
  char *dir, *base;

  dirc = strdup(path);
  basec = strdup(path);

  dir = dirname(dirc);
  base = basename(basec);

  uint64_t dir_inode_id;
  if (strcmp(dir, "/") == 0) {
    // Perform a lookup in the root directory file
    dir_inode_id = 0;
  } else {
    res = mtl_resolve_inode(txn, dir, &dir_inode_id);
    if (res != MTL_SUCCESS) {
      free(dirc), free(basec);
      return res;
    }
  }

  if (strcmp(base, "/") == 0) {
    *inode_id = 0;
    res = MTL_SUCCESS;
  } else {
    res = mtl_resolve_inode_in_directory(txn, dir_inode_id, base, inode_id);
  }

  free(dirc), free(basec);
  return res;
}

int mtl_resolve_parent_dir_inode(MDB_txn *txn, const char *path,
                                 uint64_t *inode_id) {
  int res;
  char *dirc, *dir;

  dirc = strdup(path);
  dir = dirname(dirc);

  if (strcmp(dir, "/") == 0) {
    *inode_id = 0;
    res = MTL_SUCCESS;
  } else {
    res = mtl_resolve_inode(txn, dir, inode_id);
  }

  free(dirc);
  return res;
}

int mtl_get_inode(mtl_context *context, const char *path, mtl_inode *inode) {
  MDB_txn *txn;
  mdb_txn_begin(context->env, NULL, MDB_RDONLY, &txn);

  uint64_t inode_id;
  int res = mtl_resolve_inode(txn, path, &inode_id);
  if (res != MTL_SUCCESS) {  // early exit because inode resolving failed
    mdb_txn_abort(txn);
    return res;
  }

  const mtl_inode *db_inode;
  res = mtl_load_inode(txn, inode_id, &db_inode, NULL,
                       NULL);  // NULLs because we are only interested in the
                               // inode, not the data behind it
  if (res == MTL_SUCCESS) {
    memcpy(
        inode, db_inode,
        sizeof(mtl_inode));  // need to copy because db_inode pointer points to
                             // invalid memory after aborting the DB transaction
  }

  mdb_txn_abort(txn);
  return res;
}

int mtl_open(mtl_context *context, const char *filename, uint64_t *inode_id) {
  MDB_txn *txn;
  mdb_txn_begin(context->env, NULL, MDB_RDONLY, &txn);

  uint64_t id;
  int res = mtl_resolve_inode(txn, filename, &id);

  mdb_txn_abort(txn);

  if (inode_id) *inode_id = id;

  return res;
}

int mtl_opendir(mtl_context *context, const char *filename, mtl_dir **dir) {
  MDB_txn *txn;
  mdb_txn_begin(context->env, NULL, MDB_RDONLY, &txn);

  uint64_t inode_id;
  int res = mtl_resolve_inode(txn, filename, &inode_id);

  const mtl_inode *dir_inode;
  mtl_directory_entry_head *dir_entries;
  res = mtl_load_directory(txn, inode_id, &dir_inode, &dir_entries, NULL);

  char *dir_data = malloc(sizeof(mtl_dir) + dir_inode->length);

  *dir = (mtl_dir *)dir_data;
  (*dir)->length = dir_inode->length;
  (*dir)->next = (mtl_directory_entry_head *)(dir_data + sizeof(mtl_dir));
  (*dir)->first = (*dir)->next;

  memcpy(dir_data + sizeof(mtl_dir), dir_entries, dir_inode->length);

  // we can abort because we only read
  mdb_txn_abort(txn);

  return res;
}

int mtl_readdir(mtl_context *context, mtl_dir *dir, char *buffer,
                uint64_t size) {
  (void)context;
  if (dir->next != NULL) {
    strncpy(buffer, (char *)dir->next + sizeof(mtl_directory_entry_head),
            size < dir->next->name_len ? size : dir->next->name_len);

    // Null-terminate
    if (size > dir->next->name_len) buffer[dir->next->name_len] = '\0';

    mtl_directory_entry_head *next =
        (mtl_directory_entry_head *)((char *)dir->next +
                                     sizeof(mtl_directory_entry_head) +
                                     dir->next->name_len);

    if ((char *)next + sizeof(mtl_directory_entry_head) >
        (char *)dir->first + dir->length) {
      next = NULL;
    }
    dir->next = next;

    return MTL_SUCCESS;
  }

  return MTL_COMPLETE;
}

int mtl_closedir(mtl_context *context, mtl_dir *dir) {
  (void)context;
  free(dir);
  return MTL_SUCCESS;
}

int mtl_mkdir(mtl_context *context, const char *filename, int mode) {
  int res;

  MDB_txn *txn;
  mdb_txn_begin(context->env, NULL, 0, &txn);

  uint64_t parent_dir_inode_id;
  res = mtl_resolve_parent_dir_inode(txn, filename, &parent_dir_inode_id);
  if (res != MTL_SUCCESS) {
    mdb_txn_abort(txn);
    return res;
  }

  char *basec, *base;
  basec = strdup(filename);
  base = basename(basec);

  uint64_t new_dir_inode_id;
  res = mtl_create_directory_in_directory(txn, parent_dir_inode_id, base, mode,
                                          &new_dir_inode_id);
  if (res != MTL_SUCCESS) {
    mdb_txn_abort(txn);
    free(basec);
    return res;
  }

  mdb_txn_commit(txn);
  free(basec);
  return MTL_SUCCESS;
}

int mtl_rmdir(mtl_context *context, const char *filename) {
  int res;

  MDB_txn *txn;
  mdb_txn_begin(context->env, NULL, 0, &txn);

  uint64_t parent_dir_inode_id;
  res = mtl_resolve_parent_dir_inode(txn, filename, &parent_dir_inode_id);
  if (res != MTL_SUCCESS) {
    mdb_txn_abort(txn);
    return res;
  }

  char *basec, *base;
  basec = strdup(filename);
  base = basename(basec);

  uint64_t inode_id;
  res = mtl_remove_entry_from_directory(txn, parent_dir_inode_id, base,
                                        &inode_id);
  if (res != MTL_SUCCESS) {
    mdb_txn_abort(txn);
    free(basec);
    return res;
  }

  free(basec);

  res = mtl_remove_directory(txn, inode_id);
  if (res != MTL_SUCCESS) {
    mdb_txn_abort(txn);
    return res;
  }

  mdb_txn_commit(txn);
  return MTL_SUCCESS;
}

int mtl_chown(mtl_context *context, const char *path, int uid, int gid) {
  int res;

  MDB_txn *txn;
  res = mdb_txn_begin(context->env, NULL, 0, &txn);

  uint64_t inode_id;
  res = mtl_resolve_inode(txn, path, &inode_id);
  if (res != MTL_SUCCESS) {
    mdb_txn_abort(txn);
    return -res;
  }

  const mtl_inode *old_inode;
  mtl_inode new_inode;
  const void *data;
  uint64_t data_length;
  res = mtl_load_inode(txn, inode_id, &old_inode, &data, &data_length);
  if (res != MTL_SUCCESS) {
    mdb_txn_abort(txn);
    return -res;
  }

  memcpy(&new_inode, old_inode, sizeof(mtl_inode));

  new_inode.user = uid;
  if ((int)gid >= 0) {
    new_inode.group = gid;
  }
  res = mtl_put_inode(txn, inode_id, &new_inode, data, data_length);

  mdb_txn_commit(txn);
  return res;
}

int mtl_rename(mtl_context *context, const char *from_filename,
               const char *to_filename) {
  int res;

  MDB_txn *txn;
  mdb_txn_begin(context->env, NULL, 0, &txn);

  uint64_t from_parent_dir_inode_id;
  res = mtl_resolve_parent_dir_inode(txn, from_filename,
                                     &from_parent_dir_inode_id);
  if (res != MTL_SUCCESS) {
    mdb_txn_abort(txn);
    return res;
  }

  uint64_t to_parent_dir_inode_id;
  res = mtl_resolve_parent_dir_inode(txn, to_filename, &to_parent_dir_inode_id);
  if (res != MTL_SUCCESS) {
    mdb_txn_abort(txn);
    return res;
  }

  char *from_basec, *from_base, *to_basec, *to_base;
  from_basec = strdup(from_filename);
  from_base = basename(from_basec);
  to_basec = strdup(to_filename);
  to_base = basename(to_basec);

  uint64_t inode_id;
  res = mtl_remove_entry_from_directory(txn, from_parent_dir_inode_id,
                                        from_base, &inode_id);
  if (res != MTL_SUCCESS) {
    mdb_txn_abort(txn);
    free(from_basec);
    free(to_basec);
    return res;
  }
  res = mtl_append_inode_id_to_directory(txn, to_parent_dir_inode_id, to_base,
                                         inode_id);
  if (res != MTL_SUCCESS) {
    mdb_txn_abort(txn);
    free(from_basec);
    free(to_basec);
    return res;
  }

  free(from_basec);
  free(to_basec);

  mdb_txn_commit(txn);
  return MTL_SUCCESS;
}

int mtl_create(mtl_context *context, const char *filename, int mode,
               uint64_t *inode_id) {
  int res;

  MDB_txn *txn;
  mdb_txn_begin(context->env, NULL, 0, &txn);

  uint64_t parent_dir_inode_id;
  res = mtl_resolve_parent_dir_inode(txn, filename, &parent_dir_inode_id);
  if (res != MTL_SUCCESS) {
    mdb_txn_abort(txn);
    return res;
  }

  char *basec, *base;
  basec = strdup(filename);
  base = basename(basec);

  uint64_t file_inode_id;
  res = mtl_create_file_in_directory(txn, parent_dir_inode_id, base, mode,
                                     &file_inode_id);
  if (res != MTL_SUCCESS) {
    mdb_txn_abort(txn);
    free(basec);
    return res;
  }

  mdb_txn_commit(txn);
  free(basec);

  if (inode_id) *inode_id = file_inode_id;

  return MTL_SUCCESS;
}

int mtl_expand_inode(mtl_context *context, MDB_txn *txn, uint64_t inode_id,
                     const mtl_file_extent *extents, uint64_t extents_length,
                     uint64_t size) {
  // Check how long we intend to write
  uint64_t write_end_bytes = size;
  uint64_t write_end_blocks = write_end_bytes / context->metadata.block_size;
  if (write_end_bytes % context->metadata.block_size) ++write_end_blocks;

  uint64_t current_inode_length_blocks = 0;
  mtl_file_extent last_extent = {
      .offset = 0, .length = 0};  // Used to append data if possible
  {
    for (uint64_t extent = 0; extent < extents_length; ++extent)
      current_inode_length_blocks += extents[extent].length;

    if (extents_length > 0) last_extent = extents[extents_length - 1];
  }

  while (current_inode_length_blocks < write_end_blocks) {
    // Allocate a new occupied extent with the requested length
    mtl_file_extent new_extent;
    new_extent.length = mtl_reserve_extent(
        txn, write_end_blocks - current_inode_length_blocks,
        last_extent.length ? &last_extent : NULL, &new_extent.offset, true);
    current_inode_length_blocks += new_extent.length;
    assert(new_extent
               .length);  // TODO: We don't handle "no space left on device" yet

    uint64_t new_length =
        write_end_bytes >
                current_inode_length_blocks * context->metadata.block_size
            ? current_inode_length_blocks * context->metadata.block_size
            : write_end_bytes;

    // If the new_extent offset matches the last_extent offset, we've extended
    // that last_extent
    if (last_extent.length && last_extent.offset == new_extent.offset) {
      last_extent.length += new_extent.length;
      mtl_extend_last_extent_in_file(txn, inode_id, &last_extent, new_length);
    } else {
      // Otherwise, assign the new extent to the file
      mtl_add_extent_to_file(txn, inode_id, &new_extent, new_length);
      last_extent = new_extent;
    }
  }

  return MTL_SUCCESS;
}

int mtl_write(mtl_context *context, uint64_t inode_id, const char *buffer,
              uint64_t size, uint64_t offset) {
  MDB_txn *txn;
  mdb_txn_begin(context->env, NULL, 0, &txn);

  const mtl_inode *inode;
  const mtl_file_extent *extents;
  uint64_t extents_length;
  mtl_load_file(txn, inode_id, &inode, &extents, &extents_length);

  int res = mtl_expand_inode(context, txn, inode_id, extents, extents_length,
                             offset + size);

  if (res != MTL_SUCCESS) {
    mdb_txn_abort(txn);
    return res;
  }

  mdb_txn_commit(txn);

  // Copy the actual data to storage
  context->storage->write(context, context->storage->context, inode_id, offset,
                          buffer, size);

  return MTL_SUCCESS;
}

uint64_t mtl_read(mtl_context *context, uint64_t inode_id, char *buffer,
                  uint64_t size, uint64_t offset) {
  MDB_txn *txn;
  mdb_txn_begin(context->env, NULL, MDB_RDONLY, &txn);

  uint64_t read_len = size;

  // Prepare the storage and check how much we can read
  const mtl_inode *inode;
  const mtl_file_extent *extents;
  uint64_t extents_length;
  mtl_load_file(txn, inode_id, &inode, &extents, &extents_length);

  if (inode->length < offset) {
    read_len = 0;
  } else if (inode->length < offset + size) {
    read_len -= (offset + size) - inode->length;
  }

  mdb_txn_abort(txn);

  if (read_len > 0)
    // Copy the actual data from storage
    context->storage->read(context, context->storage->context, inode_id, offset,
                           buffer, read_len);

  return read_len;
}

int mtl_truncate(mtl_context *context, uint64_t inode_id, uint64_t offset) {
  MDB_txn *txn;
  mdb_txn_begin(context->env, NULL, 0, &txn);

  const mtl_inode *inode;
  const mtl_file_extent *extents;
  uint64_t extents_length;
  mtl_load_file(txn, inode_id, &inode, &extents, &extents_length);

  if (offset >= inode->length) {
    int res = mtl_expand_inode(context, txn, inode_id, extents, extents_length,
                               offset);
    if (res != MTL_SUCCESS) {
      mdb_txn_abort(txn);
      return res;
    }
  } else {
    // Figure out how many extents we can keep
    uint64_t previous_extent_end = 0;
    for (uint64_t i = 0; i < extents_length; ++i) {
      uint64_t extent_end = previous_extent_end +
                            (extents[i].length * context->metadata.block_size);

      if (previous_extent_end > offset) {
        // The current extent has to be freed

        mtl_free_extent(txn, extents[i].offset);
      } else if (extent_end > offset) {
        // This is the first extent that has to be modified or dropped
        // Update the inode in the process

        uint64_t new_extent_length_bytes = offset - previous_extent_end;
        uint64_t new_extent_length_blocks =
            new_extent_length_bytes / context->metadata.block_size;
        if (new_extent_length_bytes % context->metadata.block_size)
          ++new_extent_length_blocks;

        if (new_extent_length_blocks) {
          // We have to modify the extent
          mtl_truncate_extent(txn, extents[i].offset, new_extent_length_blocks);
          mtl_truncate_file_extents(txn, inode_id, offset, i + 1,
                                    &new_extent_length_blocks);
        } else {
          // We can drop the extent
          mtl_free_extent(txn, extents[i].offset);
          mtl_truncate_file_extents(txn, inode_id, offset, i, NULL);
        }
      } else {
        // This extent is still valid -- keep unmodified
      }

      previous_extent_end = extent_end;
    }
  }

  mdb_txn_commit(txn);

  return MTL_SUCCESS;
}

int mtl_unlink(mtl_context *context, const char *filename) {
  // We don't (yet?) support hard links, so we can just remove the inode
  int res;

  MDB_txn *txn;
  mdb_txn_begin(context->env, NULL, 0, &txn);

  char *basec, *base;
  basec = strdup(filename);
  base = basename(basec);

  uint64_t parent_dir_inode_id;
  res = mtl_resolve_parent_dir_inode(txn, filename, &parent_dir_inode_id);

  // Remove directory entry
  uint64_t inode_id;
  res = mtl_remove_entry_from_directory(txn, parent_dir_inode_id, base,
                                        &inode_id);

  // Free all extents
  const mtl_file_extent *extents;
  uint64_t extents_length;
  res = mtl_load_file(txn, inode_id, NULL, &extents, &extents_length);

  if (res == MTL_ERROR_NOENTRY) {
    return res;
  }

  for (uint64_t i = 0; i < extents_length; ++i) {
    mtl_free_extent(txn, extents[i].offset);
  }

  // Remove inode
  mtl_delete_inode(txn, inode_id);

  mdb_txn_commit(txn);

  free(basec);

  return MTL_SUCCESS;
}

int mtl_load_extent_list(mtl_context *context, uint64_t inode_id,
                         mtl_file_extent *extents, uint64_t *extents_length,
                         uint64_t *file_length) {
  int res;

  MDB_txn *txn;
  mdb_txn_begin(context->env, NULL, MDB_RDONLY, &txn);

  // Load extents
  const mtl_inode *inode;
  const mtl_file_extent *tmp_extents;
  uint64_t tmp_extents_length;
  res = mtl_load_file(txn, inode_id, &inode, &tmp_extents, &tmp_extents_length);
  if (res != MTL_SUCCESS) {
    mdb_txn_abort(txn);
    return res;
  }

  if (tmp_extents_length > MTL_MAX_EXTENTS) {
    mdb_txn_abort(txn);
    return MTL_ERROR_INVALID_ARGUMENT;
  }

  if (extents) {
    memcpy(extents, tmp_extents, tmp_extents_length * sizeof(mtl_file_extent));
  }

  if (extents_length) {
    *extents_length = tmp_extents_length;
  }

  if (file_length) {
    *file_length = inode->length;
  }

  mdb_txn_abort(txn);
  return MTL_SUCCESS;
}
