#include <metal-filesystem/metal.h>

#include "meta.h"

#define META_DB_NAME "meta"

const char next_inode_id_key[] = "next_inode";

int mtl_ensure_meta_db_open(MDB_txn *txn, MDB_dbi *db) {
  return mdb_dbi_open(txn, META_DB_NAME, MDB_CREATE, db);
}

uint64_t mtl_next_id(MDB_txn *txn, MDB_val *key) {
  MDB_dbi meta_db;
  mtl_ensure_meta_db_open(txn, &meta_db);

  uint64_t result;
  MDB_val next_key_value;

  if (mdb_get(txn, meta_db, key, &next_key_value) == MDB_NOTFOUND) {
    result = 1;
  } else {
    result = *((uint64_t *)next_key_value.mv_data);
  }

  ++result;
  next_key_value.mv_size = sizeof(result);
  next_key_value.mv_data = &result;
  mdb_put(txn, meta_db, key, &next_key_value, 0);

  return result - 1;
}

uint64_t mtl_next_inode_id(MDB_txn *txn) {
  MDB_val key = {.mv_size = sizeof(next_inode_id_key),
                 .mv_data = (void *)&next_inode_id_key};
  return mtl_next_id(txn, &key);
}
