#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#define _TIME_BITS 64
#define _GNU_SOURCE
#include "common-config.h"
#include "common.h"
#include "xdg-base-directory.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MARKER0 "title=\""
#define MARKER1 "\"/channel/"
#define cstringcpy(dest, src)                                                                      \
  memcpy(dest, src, sizeof(src) - 1);                                                              \
  dest += sizeof(src) - 1;

#define check_ptr_cstring(ptr, cstr) (memcmp(ptr, cstr, sizeof(cstr) - 1) == 0)

struct entry {
  struct ptr_string id;
  struct ptr_string name;
  struct ptr_string en_name;
  struct ptr_string org;
  struct ptr_string open_direct;
  struct ptr_string duration;
  struct ptr_string sound;
};

internal inline u8 *find_end(u8 *start, u8 *end) {
  u8 *ret = start;
  for (; ret < end; ++ret) {
    if (*ret == '\n' || *ret == '"') {
      break;
    }
  }
  return ret;
}

internal inline u8 *stringcpy(u8 *dest, const struct ptr_string src) {
  memcpy(dest, src.start, src.end - src.start);
  return dest + (src.end - src.start);
}

internal i32 cmp_entry_by_id(const void *a, const void *b) {
  const struct entry *ea = *(struct entry **)a, *eb = *(struct entry **)b;
  return cmp_str(ea->id, eb->id);
}

internal i32 cmp_entry_by_org_then_name(const void *a, const void *b) {
  const struct entry *ea = *(struct entry **)a, *eb = *(struct entry **)b;
  i32 diff = cmp_str(ea->org, eb->org);
  if (diff != 0) {
    return diff;
  }
  return cmp_str(ea->name, eb->name);
}

internal inline struct ptr_string parse_kv(struct ptr_string field) {
  u8 *p = field.start;
  while (p < field.end && *p != '=') {
    p++;
  }
  if (p < field.end) {
    return (struct ptr_string){p + 1, field.end};
  }
  return (struct ptr_string){0, 0};
}

internal inline i32 stringeq(const struct ptr_string a, const struct ptr_string b) {
  if (a.end - a.start != b.end - b.start) {
    return 0;
  }
  return memcmp(a.start, b.start, a.end - a.start) == 0;
}

internal u32 parse_channels(u8 *channels, u64 size, struct entry *entries, u8 **first_line_end) {
  u8 *ptr = channels;
  u8 *end = channels + size;
  u8 *line_start = ptr;
  u32 entry_count = 0;
  u32 line_count = 0;

  while (ptr < end) {
    if (*ptr == '\n' || ptr == end - 1) {
      u8 *line_end = (ptr == end - 1) ? ptr + 1 : ptr;

      if (line_count > 0 && line_end - line_start > 1) {
        struct entry *current_entry = entries + entry_count;
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

        /* field 4: open_direct */
        {
          u8 *eq = scan;
          while (eq < line_end && *eq != ',') {
            eq++;
          }
          expect(check_ptr_cstring(scan, "open_direct="));
          current_entry->open_direct = parse_kv((struct ptr_string){scan, eq});
          scan = eq + 1;
        }

        /* field 5: duration */
        {
          u8 *eq = scan;
          while (eq < line_end && *eq != ',') {
            eq++;
          }
          expect(check_ptr_cstring(scan, "duration="));
          current_entry->duration = parse_kv((struct ptr_string){scan, eq});
          scan = eq + 1;
        }

        /* field 6: sound */
        {
          u8 *eq = scan;
          while (eq < line_end && *eq != ',') {
            eq++;
          }
          expect(check_ptr_cstring(scan, "sound="));
          current_entry->sound = parse_kv((struct ptr_string){scan, eq});
          scan = eq + 1;
        }

        entry_count++;
      } else if (line_count == 0) {
        *first_line_end = line_end;
      }

      line_start = ptr + 1;
      line_count++;
    }
    ptr++;
  }

  return entry_count;
}

int main(int argc, char *argv[]) {
  (void)argc, (void)argv;
  c8 config_path[PATH_MAX];
  get_file_path(config, "config.txt", config_path);
  c8 channels_path[PATH_MAX];
  get_file_path(config, "channels.csv", channels_path);
  c8 web_page_path[PATH_MAX];
  get_file_path(config, "page.html", web_page_path);
  i32 fd = open(web_page_path, O_RDONLY);
  // TODO: directory already exists but file does not
  if (fd == -1) {
    if (likely(errno == ENOENT)) {
      c8 xdg_channels_path[PATH_MAX];
      get_path_xdg_config(xdg_channels_path);
      i32 ret = mkdir(xdg_channels_path, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
      expect_errno(ret != -1, "Error creating channels directory");
      // TODO: give instruction on how to get page.html
      return 0;
    } else {
      perror("Error opening page.html");
      return 1;
    }
  }
  struct stat st;
  {
    i32 ret = fstat(fd, &st);
    expect_errno(ret != -1, "fstat page.html");
  }
  u64 size = st.st_size;
  u8 *page = mmap(0, size, PROT_READ, MAP_SHARED, fd, 0);
  expect_errno(page != MAP_FAILED, "mmap page.html");
  close(fd);

  u8 *mem = mmap(0, 128 * 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  expect_errno(mem != MAP_FAILED, "mmap memory");
  struct entry *channels_entries = (struct entry *)mem;
  u32 num_channels_entry = 0;

  u32 channels_exists = 1;
  struct entry **channels_entry_sort_order = 0;
  u8 *channels;
  u8 *first_line_end;
  i32 channels_fd = open(channels_path, O_RDONLY);
  if (channels_fd == -1) {
    expect(errno == ENOENT);
    channels_exists = 0;
  } else {
    struct stat channels_st;
    {
      i32 ret = fstat(channels_fd, &channels_st);
      expect_errno(ret != -1, "fstat channels.csv");
    }
    u64 channels_size = channels_st.st_size;
    channels = mmap(0, channels_size, PROT_READ, MAP_SHARED, channels_fd, 0);
    expect_errno(channels != MAP_FAILED, "mmap channels.csv");
    close(channels_fd);

    num_channels_entry = parse_channels(channels, channels_size, channels_entries, &first_line_end);
    mem += sizeof(struct entry) * num_channels_entry;

    struct entry **channels_entry_sort_order = (struct entry **)mem;
    for (u32 i = 0; i < num_channels_entry; ++i) {
      channels_entry_sort_order[i] = channels_entries + i;
    }
    qsort(channels_entry_sort_order, num_channels_entry, sizeof(struct entry *), cmp_entry_by_id);
    mem += sizeof(struct entry *) * num_channels_entry;
  }

  u8 *ptr = page;
  u8 *end = page + size;
  struct entry *entries = (struct entry *)mem;
  u32 num_new_entry = 0;

  while (ptr < end) {
    u8 *next = memmem(ptr, end - ptr, MARKER0, sizeof(MARKER0) - 1);
    if (next == 0) {
      break;
    }
    if (next[-2] == '"') {
      u8 *href = next - 3;
      for (; href > ptr; --href) {
        if (*href == '"') {
          break;
        }
      }
      href -= 5;
      if (href[-2] == '"' && href[0] == 'h' && href[1] == 'r' && href[2] == 'e' && href[3] == 'f') {
        expect(memcmp(href + 5, MARKER1, sizeof(MARKER1) - 1) == 0);
        struct entry *current_entry = entries + num_new_entry;
        current_entry->id.start = href + 5 + sizeof(MARKER1) - 1;
        current_entry->id.end = next - 2;
        expect(current_entry->id.end - current_entry->id.start == 24);
        u8 *existing_elem = 0;
        if (channels_exists) {
          existing_elem = bsearch(&current_entry, channels_entry_sort_order, num_channels_entry,
                                  sizeof(struct entry *), cmp_entry_by_id);
        }
        if (existing_elem == 0) {
          u8 *name_start = next + sizeof(MARKER0) - 1;
          current_entry->name.start = name_start;
          current_entry->name.end = find_end(name_start, end);
          current_entry->en_name.start = current_entry->en_name.end = 0;
          u8 *line_start = current_entry->name.end + 1;
          if (*line_start == 'E' && line_start[1] == 'N') {
            current_entry->en_name.start = line_start + 4;
            current_entry->en_name.end = find_end(current_entry->en_name.start + 1, end);
            line_start = current_entry->en_name.end + 1;
          }
          expect(*line_start == '&');
          current_entry->org.start = line_start + 5;
          current_entry->org.end = find_end(current_entry->org.start, end);
          num_new_entry++;
        }
      }
    }
    ptr = next + 1;
  };
  mem += sizeof(struct entry) * num_new_entry;

  u32 num_total_entry = num_channels_entry + num_new_entry;
  struct entry **sort_order = (struct entry **)mem;
  for (u32 i = 0; i < num_channels_entry; ++i) {
    sort_order[i] = channels_entries + i;
  }
  for (u32 i = 0; i < num_new_entry; ++i) {
    sort_order[i + num_channels_entry] = entries + i;
  }
  qsort(sort_order, num_total_entry, sizeof(struct entry *), cmp_entry_by_org_then_name);
  mem += sizeof(struct entry *) * num_total_entry;

  u8 *out_base = mem;
  u8 *out_current = out_base;
  if (channels_exists) {
    memcpy(out_current, channels, first_line_end - channels);
    out_current += first_line_end - channels;
    *(out_current++) = '\n';
  } else {
    cstringcpy(out_current, "TODO: header\n");
    // TODO: header
  }
  for (u32 i = 0; i < num_total_entry; ++i) {
    struct entry *current_entry = sort_order[i];
    if (i == 0 || !stringeq(sort_order[i - 1]->id, current_entry->id)) {
      out_current = stringcpy(out_current, current_entry->id);
      *(out_current++) = ',';
      out_current = stringcpy(out_current, current_entry->org);
      *(out_current++) = ',';
      out_current = stringcpy(out_current, current_entry->name);
      *(out_current++) = ',';
      if (current_entry->en_name.start != 0 && current_entry->en_name.end != 0) {
        out_current = stringcpy(out_current, current_entry->en_name);
      }
      cstringcpy(out_current, ",open_direct=");
      if (current_entry->open_direct.start == 0 || current_entry->open_direct.end == 0) {
        *(out_current++) = '?';
      } else {
        out_current = stringcpy(out_current, current_entry->open_direct);
      }
      cstringcpy(out_current, ",duration=");
      if (current_entry->duration.start == 0 || current_entry->duration.end == 0) {
        *(out_current++) = '?';
      } else {
        out_current = stringcpy(out_current, current_entry->duration);
      }
      cstringcpy(out_current, ",sound=");
      if (current_entry->sound.start == 0 || current_entry->sound.end == 0) {
        *(out_current++) = '?';
      } else {
        out_current = stringcpy(out_current, current_entry->sound);
      }
      *(out_current++) = '\n';
    }
  }
  i32 out_fd =
      open(channels_path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  expect_errno(out_fd != -1, "Error opening channels.csv");
  i64 ret = write(out_fd, out_base, out_current - out_base);
  expect(ret == out_current - out_base);
  close(out_fd);
  {
    i32 ret = access(config_path, R_OK);
    if (ret == -1) {
      if (errno == ENOENT) {
        i32 config_fd =
            open(config_path, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        expect_errno(config_fd != -1, "Error creating config.txt");
        write(config_fd, CONFIG_TXT_DEFAULT, sizeof(CONFIG_TXT_DEFAULT));
        close(config_fd);
      } else {
        perror("Error opening config.txt");
        return 1;
      }
    } else {
      return 1;
    }
  }
  return 0;
}
