#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#define _TIME_BITS 64
#define _GNU_SOURCE
#include "common-config.h"
#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define PREFIX "includePlaceholder=true&channels="
#define SEP "%2C"

#define warn_bad_key_value(key, val)                                                               \
  fprintf(stderr, "Warning: invalid %s value '%.*s', using default value\n", key,                  \
          (i32)(val.end - val.start), val.start)

struct entry {
  struct ptr_string id;
  struct ptr_string name;
  struct ptr_string en_name;
  struct ptr_string org;
  i32 open_direct;
  i32 duration;
  struct ptr_string sound;
};

internal i32 cmp_entry_by_id(const void *a, const void *b) {
  const struct entry *ea = *(struct entry **)a, *eb = *(struct entry **)b;
  return cmp_str(ea->id, eb->id);
}

#define cstringeq(start, end, str)                                                                 \
  (sizeof(str) - 1 == end - start ? memcmp(start, str, end - start) == 0 : 0)

internal inline i32 parse_open_direct(const struct ptr_string val) {
  if ((val.end - val.start == 1) && *val.start == '?') {
    return -1;
  }
  if ((val.end - val.start == 1) && (*val.start == '0' || *val.start == '1')) {
    return (i32)(*val.start - '0');
  }
  warn_bad_key_value("open_direct", val);
  return -1;
}

internal inline i32 parse_duration(const struct ptr_string val) {
  if (val.end - val.start == 1 && *val.start == '?') {
    return -1;
  }
  c8 *endptr;
  i64 result = strtoll((const c8 *)val.start, &endptr, 10);
  if (result < 0 || result != (u32)result) {
    warn_bad_key_value("duration", val);
    return -1;
  }
  if ((u8 *)endptr == val.end - 1) {
    if (*endptr == 's') {
      return result * 1000;
    } else if (*endptr == 'm') {
      return result * 60 * 1000;
    } else if (*endptr == 'h') {
      return result * 60 * 60 * 1000;
    } else {
      fprintf(stderr, "Error: invalid duration postfix '%c' in '%.*s', expected 's', 'm', or 'h'\n",
              *endptr, (i32)(val.end - val.start), val.start);
      exit(1);
    }
  } else if ((u8 *)endptr == val.end) {
    // default to 's' postfix if none is found
    return result * 1000;
  } else {
    fprintf(stderr, "Error: invalid duration value '%.*s', contains unexpected characters\n",
            (i32)(val.end - val.start), val.start);
    exit(1);
  }
}

internal inline struct ptr_string parse_sound(const struct ptr_string val) {
  if (val.end - val.start == 1 && *val.start == '?') {
    struct ptr_string ret = {0, 0};
    return ret;
  }
  return val;
}

internal u32 parse_config(u8 *config, u64 size, struct arena *arena,
                          struct config_file_defaults *parsed_default,
                          struct ptr_string *sound_str) {
  u8 *ptr = config;
  u8 *end = config + size;
  u8 *line_start = ptr;
  u32 entry_count = 0;
  u32 line_count = 0;

  while (ptr < end) {
    if (*ptr == '\n' || ptr == end - 1) {
      u8 *line_end = (ptr == end - 1) ? ptr + 1 : ptr;

      if (line_count > 0 && line_end - line_start > 1) {
        struct entry *current_entry = push(arena, sizeof(struct entry));
        u8 *scan = line_start;

        /* field 0: id */
        current_entry->id.start = scan;
        while (scan < line_end && *scan != ',') {
          scan++;
        }
        current_entry->id.end = scan;
        scan++;

        /* field 1: org */
        current_entry->org.start = scan;
        while (scan < line_end && *scan != ',') {
          scan++;
        }
        current_entry->org.end = scan;
        scan++;

        /* field 2: name */
        current_entry->name.start = scan;
        while (scan < line_end && *scan != ',') {
          scan++;
        }
        current_entry->name.end = scan;
        scan++;

        /* field 3: en_name */
        current_entry->en_name.start = scan;
        while (scan < line_end && *scan != ',') {
          scan++;
        }
        current_entry->en_name.end = scan;
        scan++;

        {
          u8 *eq = scan;
          while (eq < line_end && *eq != ',') {
            eq++;
          }
          struct ptr_string field = {scan, eq};
          u8 *dv = field.start;
          while (dv < field.end && *dv != '=') {
            dv++;
          }
          expect(cstringeq(field.start, dv + 1, "open_direct="));
          current_entry->open_direct = parse_open_direct((struct ptr_string){dv + 1, field.end});
          scan = eq + 1;
        }

        {
          u8 *eq = scan;
          while (eq < line_end && *eq != ',') {
            eq++;
          }
          struct ptr_string field = {scan, eq};
          u8 *dv = field.start;
          while (dv < field.end && *dv != '=') {
            dv++;
          }
          expect(cstringeq(field.start, dv + 1, "duration="));
          current_entry->duration = parse_duration((struct ptr_string){dv + 1, field.end});
          scan = eq + 1;
        }

        {
          u8 *eq = scan;
          while (eq < line_end && *eq != ',') {
            eq++;
          }
          struct ptr_string field = {scan, eq};
          u8 *dv = field.start;
          while (dv < field.end && *dv != '=') {
            dv++;
          }
          expect(cstringeq(field.start, dv + 1, "sound="));
          current_entry->sound = parse_sound((struct ptr_string){dv + 1, field.end});
        }

        entry_count++;
      } else if (line_count == 0) {
        u8 *scan = line_start;
        {
          u8 *eq = scan;
          while (eq < line_end && *eq != ',') {
            eq++;
          }
          struct ptr_string field = {scan, eq};
          u8 *dv = field.start;
          while (dv < field.end && *dv != '=') {
            dv++;
          }
          expect(cstringeq(field.start, dv + 1, "open_direct="));
          if ((field.end - (dv + 1) == 1) && (*(dv + 1) == '0' || *(dv + 1) == '1')) {
            parsed_default->open_direct = (u32)(*(dv + 1) - '0');
          } else {
            fprintf(stderr,
                    "Error: invalid open_direct default value '%.*s', expected '0' or '1'\n",
                    (i32)(field.end - (dv + 1)), dv + 1);
            exit(1);
          }
          scan = eq + 1;
        }

        {
          u8 *eq = scan;
          while (eq < line_end && *eq != ',') {
            eq++;
          }
          struct ptr_string field = {scan, eq};
          u8 *dv = field.start;
          while (dv < field.end && *dv != '=') {
            dv++;
          }
          expect(cstringeq(field.start, dv + 1, "duration="));
          c8 *endptr;
          i64 result = strtoll((c8 *)(dv + 1), &endptr, 10);
          if (result < 0 || result != (u32)result) {
            fprintf(
                stderr,
                "Error: invalid duration default value '%.*s', not a valid non-negative integer\n",
                (i32)(field.end - (dv + 1)), dv + 1);
            exit(1);
          }
          if ((u8 *)endptr == field.end - 1) {
            if (*endptr == 's') {
              parsed_default->duration = result * 1000;
            } else if (*endptr == 'm') {
              parsed_default->duration = result * 60 * 1000;
            } else if (*endptr == 'h') {
              parsed_default->duration = result * 60 * 60 * 1000;
            } else {
              fprintf(stderr,
                      "Error: invalid duration postfix '%c' in '%.*s', expected 's', 'm', or 'h'\n",
                      *endptr, (i32)(field.end - (dv + 1)), dv + 1);
              exit(1);
            }
          } else if ((u8 *)endptr == field.end) {
            // default to seconds
            parsed_default->duration = result * 1000;
          } else {
            fprintf(
                stderr,
                "Error: invalid duration default value '%.*s', contains unexpected characters\n",
                (i32)(field.end - (dv + 1)), dv + 1);
            exit(1);
          }
          scan = eq + 1;
        }

        {
          u8 *eq = scan;
          while (eq < line_end && *eq != ',') {
            eq++;
          }
          struct ptr_string field = {scan, eq};
          u8 *dv = field.start;
          while (dv < field.end && *dv != '=') {
            dv++;
          }
          expect(cstringeq(field.start, dv + 1, "sound="));
          sound_str->start = dv + 1;
          sound_str->end = field.end;
        }
      }

      line_start = ptr + 1;
      line_count++;
    }
    ptr++;
  }

  return entry_count;
}

internal inline struct offset_string cpy_field(struct arena *arena, struct ptr_string str) {
  u32 len = str.end - str.start + 1;
  u8 *ptr = push(arena, len);
  memcpy(ptr, str.start, len - 1);
  ptr[len - 1] = 0;
  struct offset_string ret = {.offset = ptr - (u8 *)arena->mem, .len = len};
  return ret;
}

int main(int argc, char *argv[]) {
  (void)argc, (void)argv;
  i32 config_fd = open("./config.txt", O_RDONLY);
  if (config_fd == -1) {
    if (errno == ENOENT) {
      fprintf(stderr, "Error: config.txt not found, run setup-config first\n");
      return 1;
    }
    perror("Error opening config.txt");
    return 1;
  }
  struct stat config_st;
  {
    i32 ret = fstat(config_fd, &config_st);
    expect_errno(ret != -1, "fstat config.txt");
  }
  u64 config_size = config_st.st_size;
  u8 *config = mmap(0, config_size, PROT_READ, MAP_SHARED, config_fd, 0);
  expect_errno(config != MAP_FAILED, "mmap config.txt");

  u8 *mem = mmap(0, 128 * 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  expect_errno(mem != MAP_FAILED, "mmap memory");
  struct arena arena_ = {.mem = mem, .current = mem, .max = mem + 128 * 4096};
  struct arena *arena = &arena_;
  struct entry *config_entries = (struct entry *)arena->current;

  struct config_file_defaults parsed_defaults = {};
  struct ptr_string sound_str;
  u32 num_config_entry = parse_config(config, config_size, arena, &parsed_defaults, &sound_str);

  if (unlikely(num_config_entry == 0)) {
    fprintf(stderr, "Error: no entries found in config.txt\n");
    return 1;
  }

  struct entry **config_entry_sort_order =
      (struct entry **)push(arena, num_config_entry * sizeof(struct entry *));
  for (u32 i = 0; i < num_config_entry; ++i) {
    config_entry_sort_order[i] = config_entries + i;
  }
  qsort(config_entry_sort_order, num_config_entry, sizeof(struct entry *), cmp_entry_by_id);

  u8 *begin = push(arena, sizeof(struct config_file_header));
  struct config_file_header *header = (struct config_file_header *)begin;
  header->magic = CONFIG_FILE_MAGIC;
  header->version = 1;
  header->num_entry = num_config_entry;

  struct config_file_entry *file_entries =
      push(arena, num_config_entry * sizeof(struct config_file_entry));

  struct config_file_defaults *file_defaults = push(arena, sizeof(struct config_file_defaults));
  file_defaults->open_direct = parsed_defaults.open_direct;
  file_defaults->duration = parsed_defaults.duration;

  u32 query_str_len = 0;
  {
    u8 *prefix = push(arena, sizeof(PREFIX) - 1);
    query_str_len += sizeof(PREFIX) - 1;
    memcpy(prefix, PREFIX, sizeof(PREFIX) - 1);
    for (u32 i = 0; i < num_config_entry - 1; ++i) {
      struct entry *entry = config_entries + i;
      u8 *id = push(arena, entry->id.end - entry->id.start + 3);
      query_str_len += entry->id.end - entry->id.start + 3;
      memcpy(id, entry->id.start, entry->id.end - entry->id.start);
      id += entry->id.end - entry->id.start;
      memcpy(id, SEP, sizeof(SEP) - 1);
    }
    struct entry *entry = config_entries + num_config_entry - 1;
    u8 *id = push(arena, entry->id.end - entry->id.start + 1);
    query_str_len += entry->id.end - entry->id.start + 1;
    memcpy(id, entry->id.start, entry->id.end - entry->id.start);
    id += entry->id.end - entry->id.start;
    id[0] = 0;
  }
  header->query_str_len = query_str_len;

  struct arena string_arena_ = {
      .mem = arena_.current, .current = arena_.current, .max = arena_.max};
  struct arena *string_arena = &string_arena_;
  {
    for (u32 i = 0; i < num_config_entry; ++i) {
      struct entry *entry = config_entry_sort_order[i];
      struct config_file_entry *file_entry = file_entries + i;
      file_entry->id = cpy_field(string_arena, entry->id);
      file_entry->name = cpy_field(string_arena, entry->name);
      file_entry->en_name = cpy_field(string_arena, entry->en_name);
      file_entry->org = cpy_field(string_arena, entry->org);
      file_entry->open_direct = entry->open_direct;
      file_entry->duration = entry->duration;
      file_entry->sound = cpy_field(string_arena, entry->sound);
    }
  }
  file_defaults->sound = cpy_field(string_arena, sound_str);
  header->string_pool_len = (u8 *)string_arena_.current - (u8 *)string_arena_.mem;

  i32 out_fd =
      open("./config.bin", O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  expect_errno(out_fd != -1, "Error opening config.bin");
  i32 ret = write(out_fd, begin, (u8 *)string_arena_.current - begin);
  expect(ret == (u8 *)string_arena_.current - begin);
  close(out_fd);
  return 0;
}
