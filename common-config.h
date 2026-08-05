#ifndef COMMON_CONFIG_H_
#define COMMON_CONFIG_H_
#include "common.h"
#include <string.h>

#define CONFIG_FILE_MAGIC (*(u64 *)"ziwu07nc")

struct ptr_string {
  u8 *start;
  u8 *end;
};

struct __attribute__((__packed__)) offset_string {
  u32 offset;
  u32 len;
};

/*
 * file format:
 * header
 * entries
 * defaults
 * query string
 * string pool
 */

struct __attribute__((__packed__)) config_file_header {
  u64 magic;
  u32 version;
  u32 num_entry;
  u32 query_str_len;
  u32 string_pool_len;
};

struct __attribute__((__packed__)) config_file_entry {
  struct offset_string id;
  struct offset_string name;
  struct offset_string en_name;
  struct offset_string org;
  i32 open_direct;
  i32 duration;
  struct offset_string sound;
};

struct __attribute__((__packed__)) config_file_defaults {
  i32 open_direct;
  i32 duration;
  struct offset_string sound;
};

internal inline i32 cmp_str(const struct ptr_string a, const struct ptr_string b) {
  i32 len_a = (i32)(a.end - a.start);
  i32 len_b = (i32)(b.end - b.start);
  u32 cmp_len = (u32)(len_a < len_b ? len_a : len_b);
  i32 diff = memcmp(a.start, b.start, cmp_len);
  return diff ? diff : len_a - len_b;
}

#endif
