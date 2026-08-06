#ifndef SESSION_RESTORE_H_
#define SESSION_RESTORE_H_
#include "common.h"

#define SESSION_FILE_MAGIC (*(u64 *)"ziwu07ns")

/*
 * session file format:
 * header
 * entries
 * string pool
 */

struct __attribute__((__packed__)) session_file_header {
  u64 magic;
  u32 version;
  u32 num_entry;
};

struct __attribute__((__packed__)) session_file_entry {
  struct offset_string id;
};

struct session_file {
  u8 *base;
  struct session_file_entry *entries;
  u8 *string_pool;
  u32 file_size;
  u32 num_entry;
};

#endif
