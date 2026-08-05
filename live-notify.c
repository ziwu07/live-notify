#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#define _TIME_BITS 64
#define JSMN_PARENT_LINKS
#include "common-config.h"
#include "common.h"
#include "jsmn.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#define check_CURLcode()                                                                           \
  {                                                                                                \
    if (unlikely(curlcode != CURLE_OK)) {                                                          \
      curl_easy_cleanup(curl);                                                                     \
      curl_global_cleanup();                                                                       \
      fprintf(stderr, "Error at %u: %u\n", __LINE__, curlcode);                                    \
      return 1;                                                                                    \
    }                                                                                              \
  }

#define check_CURLUcode()                                                                          \
  {                                                                                                \
    if (unlikely(curlucode != CURLUE_OK)) {                                                        \
      curl_url_cleanup(url);                                                                       \
      curl_easy_cleanup(curl);                                                                     \
      curl_global_cleanup();                                                                       \
      fprintf(stderr, "Error at %u: %u\n", __LINE__, curlucode);                                   \
      return 1;                                                                                    \
    }                                                                                              \
  }

#define check_ret_syscall(msg)                                                                     \
  {                                                                                                \
    if (unlikely(ret == -1)) {                                                                     \
      perror(msg);                                                                                 \
      return 1;                                                                                    \
    }                                                                                              \
  }

struct api_data {
  u8 *buf;
  u64 size;
};

struct live_status {
  u8 *title;
  u8 *id;           // do not use if link is not null
  u8 *start_actual; // use available_at instead if empty
  u8 *available_at;
  u8 *topic; // could be empty
  u8 *link;  // for external streams only, empty otherwise
  u8 *channel_id;
  u8 *name;
  u8 *photo_url;
  u8 *photo_path;
  u32 title_len;
  u32 id_len;
  u32 start_actual_len;
  u32 available_at_len;
  u32 topic_len;
  u32 link_len;
  u32 channel_id_len;
  u32 name_len;
  u32 photo_url_len;
  u32 photo_path_len;
};

struct notification {
  u32 id;
  u8 link[LINK_MAX_LEN];
};

struct buff_string {
  u32 start;
  u32 end;
};

struct live_status_ptr {
  struct buff_string obj;
  struct buff_string channel;
  struct buff_string title;
  struct buff_string id;           // do not use if link is not null
  struct buff_string start_actual; // use available_at instead if null
  struct buff_string available_at;
  struct buff_string topic; // could be null
  struct buff_string link;  // for external streams only, null otherwise
  struct buff_string channel_id;
  struct buff_string name;
  struct buff_string photo_url;
};

struct parsed_config_file {
  struct config_file_entry *entries;
  struct config_file_defaults *defaults;
  c8 *query_str;
  u8 *string_pool;
  u32 num_entry;
};

internal u32 basename_start(u8 *buf, struct buff_string str) {
  for (u32 i = str.end - 1; i >= str.start; --i) {
    if (buf[i] == '/') {
      return i + 1;
    }
  }
  return str.end - 1;
}

internal inline void stringcpy(const u8 *const restrict buf, struct buff_string str,
                               u8 *const restrict dst) {
  memcpy(dst, buf + str.start, str.end - str.start);
  dst[str.end - str.start] = 0;
}

internal struct live_status *parse_json(struct arena *arena, c8 *pfp_path, u32 pfp_path_len,
                                        struct api_data *data, void *json_buf,
                                        struct live_status_ptr *intermediate, u32 *num_live_out) {
  jsmn_parser parser;
  jsmn_init(&parser);
  jsmntok_t *tokens = json_buf;
  u8 *buf = data->buf;
  u64 size = data->size;
  i32 ret = jsmn_parse(&parser, (c8 *)buf, size, tokens, (JSON_BUF_MAX / sizeof(jsmntok_t)));
  if (unlikely(ret < 0)) {
    switch (ret) {
    case JSMN_ERROR_INVAL:
      fprintf(stderr, "Error: JSON parsing failed — invalid JSON input\n");
      break;
    case JSMN_ERROR_NOMEM:
      fprintf(stderr, "Error: JSON parsing failed — not enough tokens allocated\n");
      break;
    case JSMN_ERROR_PART:
      fprintf(stderr, "Error: JSON parsing failed — truncated JSON\n");
      break;
    }
    return 0;
  }
  u32 num_token = (u32)ret;
  expect(tokens[0].type == JSMN_ARRAY);
  u32 num_total_exp = tokens[0].size;
  expect(tokens[1].type == JSMN_OBJECT);
  u32 num_total_res = 0;
  u32 num_live = 0;
  u32 does_inc = 0;
  struct live_status_ptr *status = intermediate;
  memset(status, 0, num_total_exp * sizeof(struct live_status_ptr));
  jsmntok_t *last_stream_obj;

  for (u32 i = 0; i < num_token; ++i) {
    jsmntok_t *token = tokens + i;
    u32 len = token->end - token->start;
    if (token->type == JSMN_STRING) {
      u8 *str = buf + token->start;
      if (len == 6 && memcmp(str, "status", 6) == 0) {
        num_total_res++;
        jsmntok_t *next_tok = tokens + i + 1;
        u32 next_len = next_tok->end - next_tok->start;
        u8 *next_str = buf + next_tok->start;
        if (next_len == 4 && memcmp(next_str, "live", 4) == 0) {
          status[num_live].obj.start = tokens[token->parent].start;
          status[num_live].obj.end = tokens[token->parent].end;
          does_inc = 1;
        }
      }
    } else if (token->type == JSMN_OBJECT) {
      jsmntok_t *last = tokens + i - 1;
      if (last->type == JSMN_STRING && last->end - last->start == 7 &&
          memcmp(buf + last->start, "channel", 7) == 0) {
        status[num_live].channel.start = token->start;
        status[num_live].channel.end = token->end;
      } else if (i == 1 || token->start == last_stream_obj->end + 1) {
        last_stream_obj = token;
        if (does_inc) {
          num_live++;
          does_inc = 0;
        }
      }
    }
  }
  if (does_inc) {
    num_live++;
  }

  expect(num_total_exp == num_total_res);
  expect(num_live <= NUM_LIVE_MAX);

  u32 idx = 0;
  for (u32 i = 0; i < num_token; ++i) {
    jsmntok_t *token = tokens + i;
    u32 len = token->end - token->start;
    struct live_status_ptr *current_live_status = status + idx;
    // printf("%u vs %u\n", token->start, current_live_status->obj.end);
    if ((u32)token->start >= current_live_status->obj.end) {
      idx++;
      if (idx == num_live) {
        break;
      }
      current_live_status = status + idx;
    }
    if ((u32)token->start < current_live_status->obj.start) {
      continue;
    }
    if (token->size == 1 && token->type == JSMN_STRING) {
      jsmntok_t *next = tokens + i + 1;
      if (len == 5 && memcmp(buf + token->start, "title", 5) == 0) {
        current_live_status->title.start = next->start;
        current_live_status->title.end = next->end;
      } else if (len == 2 && (u32)tokens[token->parent].start == current_live_status->obj.start &&
                 (u32)tokens[token->parent].end == current_live_status->obj.end &&
                 memcmp(buf + token->start, "id", 2) == 0) {
        current_live_status->id.start = next->start;
        current_live_status->id.end = next->end;
      } else if (len == 2 &&
                 (u32)tokens[token->parent].start == current_live_status->channel.start &&
                 (u32)tokens[token->parent].end == current_live_status->channel.end &&
                 memcmp(buf + token->start, "id", 2) == 0) {
        current_live_status->channel_id.start = next->start;
        current_live_status->channel_id.end = next->end;
      } else if (len == 12 && memcmp(buf + token->start, "start_actual", 12) == 0) {
        current_live_status->start_actual.start = next->start;
        current_live_status->start_actual.end = next->end;
      } else if (len == 12 && memcmp(buf + token->start, "available_at", 12) == 0) {
        current_live_status->available_at.start = next->start;
        current_live_status->available_at.end = next->end;
      } else if (len == 8 && memcmp(buf + token->start, "topic_id", 8) == 0) {
        current_live_status->topic.start = next->start;
        current_live_status->topic.end = next->end;
      } else if (len == 4 && (u32)tokens[token->parent].start == current_live_status->obj.start &&
                 (u32)tokens[token->parent].end == current_live_status->obj.end &&
                 memcmp(buf + token->start, "link", 4) == 0) {
        current_live_status->link.start = next->start;
        current_live_status->link.end = next->end;
      } else if (len == 4 &&
                 (u32)tokens[token->parent].start == current_live_status->channel.start &&
                 (u32)tokens[token->parent].end == current_live_status->channel.end &&
                 memcmp(buf + token->start, "name", 4) == 0) {
        current_live_status->name.start = next->start;
        current_live_status->name.end = next->end;
      } else if (len == 5 && memcmp(buf + token->start, "photo", 5) == 0) {
        current_live_status->photo_url.start = next->start;
        current_live_status->photo_url.end = next->end;
      }
    }
  }

  struct live_status *live = push_align(arena, num_live * sizeof(struct live_status));
  for (u32 i = 0; i < num_live; ++i) {

    live[i].title_len = status[i].title.end - status[i].title.start + 1;
    live[i].title = push(arena, live[i].title_len);
    stringcpy(buf, status[i].title, live[i].title);

    live[i].id_len = status[i].id.end - status[i].id.start + 1;
    live[i].id = push(arena, live[i].id_len);
    stringcpy(buf, status[i].id, live[i].id);

    live[i].start_actual_len = status[i].start_actual.end - status[i].start_actual.start + 1;
    live[i].start_actual = push(arena, live[i].start_actual_len);
    stringcpy(buf, status[i].start_actual, live[i].start_actual);

    live[i].available_at_len = status[i].available_at.end - status[i].available_at.start + 1;
    live[i].available_at = push(arena, live[i].available_at_len);
    stringcpy(buf, status[i].available_at, live[i].available_at);

    live[i].topic_len = status[i].topic.end - status[i].topic.start + 1;
    live[i].topic = push(arena, live[i].topic_len);
    stringcpy(buf, status[i].topic, live[i].topic);

    live[i].link_len = status[i].link.end - status[i].link.start + 1;
    live[i].link = push(arena, live[i].link_len);
    stringcpy(buf, status[i].link, live[i].link);

    live[i].channel_id_len = status[i].channel_id.end - status[i].channel_id.start + 1;
    live[i].channel_id = push(arena, live[i].channel_id_len);
    stringcpy(buf, status[i].channel_id, live[i].channel_id);

    live[i].name_len = status[i].name.end - status[i].name.start + 1;
    live[i].name = push(arena, live[i].name_len);
    stringcpy(buf, status[i].name, live[i].name);

    live[i].photo_url_len = status[i].photo_url.end - status[i].photo_url.start + 1;
    live[i].photo_url = push(arena, live[i].photo_url_len);
    stringcpy(buf, status[i].photo_url, live[i].photo_url);

    u32 start = basename_start(buf, status[i].photo_url);
    struct buff_string pfp_id = {.start = start, .end = status[i].photo_url.end};
    live[i].photo_path_len = pfp_path_len + pfp_id.end - pfp_id.start + 1;
    live[i].photo_path = push(arena, live[i].photo_path_len);
    memcpy(live[i].photo_path, pfp_path, pfp_path_len);
    stringcpy(buf, pfp_id, live[i].photo_path + pfp_path_len);
  }
  *num_live_out = num_live;

  return live;
}

internal u64 write_callback(u8 *buf, u64 size, u64 len, void *userdata) {
  if (unlikely(size != 1)) {
    return 0;
  }
  struct api_data *data = (struct api_data *)userdata;
  if (unlikely(data->size + len > API_DATA_SIZE)) {
    return 0;
  }
  memcpy(data->buf + data->size, buf, len);
  data->size += len;
  return len;
}

internal u32 previous_notification = 0;

internal i32 action_invoked(sd_bus_message *m, void *userdata, sd_bus_error *reterr_error) {
  (void)reterr_error;
  struct notification *notify_list = (struct notification *)userdata;
  u32 id;
  sd_bus_message_read_basic(m, 'u', &id);
  // u8 *action;
  // sd_bus_message_read_basic(m, 's', &action);
  // printf("called: %u, %s\n", id, action);
  i32 idx = -1;
  for (u32 i = 0; i < NUM_NOTIFICATION_MAX; ++i) {
    if (notify_list[i].id == id) {
      idx = i;
      previous_notification = i;
      notify_list[i].id = 0;
      break;
    }
  }
  if (unlikely(idx == -1)) {
    printf("Warning: Notification data gone\n");
  } else {
    u64 pid = fork();
    if (pid == 0) {
      i32 fd = open("/dev/null", O_WRONLY);
      if (likely(fd != -1)) {
        dup2(fd, STDOUT_FILENO);
      }
      c8 *argv0 = "/usr/bin/brave";
      c8 *argv[] = {argv0, (c8 *)notify_list[idx].link, 0};
      execv(argv0, argv);
    }
  }
  return 0;
}

internal i32 notification_closed(sd_bus_message *m, void *userdata, sd_bus_error *reterr_error) {
  (void)reterr_error;
  struct notification *notify_list = (struct notification *)userdata;
  u32 id;
  sd_bus_message_read_basic(m, 'u', &id);
  if (id == previous_notification) {
    return 0;
  }
  for (u32 i = 0; i < NUM_NOTIFICATION_MAX; ++i) {
    if (notify_list[i].id == id) {
      notify_list[i].id = 0;
      return 0;
    }
  }
  return 0;
}

internal inline struct parsed_config_file parse_config(u8 *config_file, i64 file_size) {
  struct parsed_config_file res;
  struct config_file_header *header = (struct config_file_header *)config_file;
  if (unlikely(header->magic != CONFIG_FILE_MAGIC)) {
    fprintf(stderr, "Error: config.bin has invalid magic — regenerate with apply-config\n");
    exit(1);
  }
  if (unlikely(header->version != 1)) {
    fprintf(
        stderr,
        "Error: config.bin version %d is unsupported (expected 1) — regenerate with apply-config\n",
        header->version);
    exit(1);
  }
  res.entries = (struct config_file_entry *)(header + 1);
  res.num_entry = header->num_entry;
  res.defaults = (struct config_file_defaults *)(res.entries + header->num_entry);
  res.query_str = (c8 *)(res.defaults + 1);
  res.string_pool = (u8 *)(res.query_str + header->query_str_len);
  u8 *end = res.string_pool + header->string_pool_len;
  expect(file_size == (end - config_file));
  return res;
}

internal inline struct config_file_entry *bsearch_by_id(struct parsed_config_file *list, u8 *id,
                                                        u32 id_len) {
  u32 low = 0;
  u32 high = list->num_entry - 1;
  do {
    u32 mid = (high + low) / 2;
    i32 ret = memcmp(id, list->string_pool + list->entries[mid].id.offset, id_len);
    if (ret > 0) {
      low = mid + 1;
    } else if (ret < 0) {
      high = mid - 1;
    } else {
      return list->entries + mid;
    }
  } while (low < high);
  if (memcmp(id, list->string_pool + list->entries[low].id.offset, id_len) == 0) {
    return list->entries + low;
  }
  return 0;
}

internal volatile sig_atomic_t quit = 0;

internal void signal_handler(i32 signal) {
  (void)signal;
  quit = 1;
}

int main(int argc, char *argv[]) {
  (void)argc, (void)argv;

  struct arena _arena;
  struct arena *arena = &_arena;
  {
    arena->mem = arena->current =
        mmap(NULL, ALLOC_MEM, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (unlikely(arena->mem == MAP_FAILED)) {
      return 1;
    }
    memset(arena->mem, 0, ALLOC_MEM);
    arena->max = ((u8 *)arena->mem) + ALLOC_MEM - 1;
  }

  // api key
  c8 x_api[10 + 37 + 1] = "X-APIKEY: ";
  {
    i32 fd = open(".env", O_RDONLY);
    if (fd == -1) {
      fprintf(stderr, "Error: .env not found\n");
      return 1;
    }
    i32 ret = read(fd, x_api + 10, 37);
    expect(ret == 37);
    x_api[10 + 37] = 0;
    close(fd);
  }

  // channel ids
  i32 config_fd;
  u64 last_mtime;
  u8 *config_file;
  struct parsed_config_file config;
  {
    config_fd = open("./config.bin", O_RDONLY);
    if (config_fd == -1) {
      perror("Error opening config.bin");
      return 1;
    }
    struct stat st = {};
    {
      i32 ret = fstat(config_fd, &st);
      check_ret_syscall("Failed to get config.bin");
    }
    i64 file_size = st.st_size;
    expect(file_size <= CHANNELS_FILE_MAX);
    last_mtime = st.st_mtime;
    config_file = push_align(arena, CHANNELS_FILE_MAX);
    i64 ret = read(config_fd, config_file, file_size);
    check_ret_syscall("Failed to read config.bin");
    expect(ret == file_size);
    config = parse_config(config_file, file_size);
  }

  c8 pfp_path[PATH_MAX];
  u32 pfp_path_len = 0;
  {
    c8 *cwd_ret = getcwd(pfp_path, PATH_MAX - sizeof(PFP_REL));
    if (unlikely(!cwd_ret)) {
      perror("Failed to get working directory.");
      return 1;
    }
    pfp_path_len = strlen(pfp_path);
    memcpy(pfp_path + strlen(pfp_path), PFP_REL, sizeof(PFP_REL));
    pfp_path_len += sizeof(PFP_REL) - 1;
  }

  {
    i32 ret = access(pfp_path, R_OK | W_OK | X_OK);
    if (unlikely(ret == -1)) {
      if (likely(errno == ENOENT)) {
        ret = mkdir(pfp_path, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
        check_ret_syscall("Failed to create directory ./pfp");
        ret = access("./pfp/", R_OK | W_OK | X_OK);
        check_ret_syscall("Failed to access ./pfp");
      } else {
        perror("Failed to access file in ./pfp/");
        return 1;
      }
    }
  }

  // timerfd
  i32 tfd;
  {
    tfd = timerfd_create(CLOCK_REALTIME, 0);
    if (unlikely(tfd == -1)) {
      perror("Failed to create timer");
      return 1;
    }
    struct timespec ts;
    i32 ret;
    ret = clock_gettime(CLOCK_REALTIME, &ts);
    check_ret_syscall("Failed to get current time");
    // ts.tv_sec = (ts.tv_sec / 60 + DELAY_MINUTE) * 60;
    // ts.tv_nsec = 0;
    ts.tv_sec = ts.tv_sec + 10;
    ts.tv_nsec = 0;
    struct itimerspec its;
    its.it_value = ts;
    its.it_interval.tv_sec = DELAY_MINUTE * 60;
    its.it_interval.tv_nsec = 0;

    ret = timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, 0);
    check_ret_syscall("Failed to set timer");
  }

  // curl
  CURLcode curlcode;
  CURL *curl;
  CURL *curl_download;
  CURLU *url;
  CURLUcode curlucode;
  c8 curl_error[CURL_ERROR_SIZE];
  struct curl_slist *headers;
  struct api_data data = {0};
  c8 youtube_favicon[PATH_MAX];
  c8 twitch_favicon[PATH_MAX];
  {
    curlcode = curl_global_init(CURL_GLOBAL_ALL);
    if (unlikely(curlcode != CURLE_OK)) {
      fprintf(stderr, "Error: %u\n", __LINE__);
      return 1;
    }
    curl = curl_easy_init();
    if (unlikely(curl == 0)) {
      curl_global_cleanup();
      fprintf(stderr, "Error: %u\n", __LINE__);
      return 1;
    }
    curlcode = curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    check_CURLcode();
    curlcode = curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    check_CURLcode();
    curlcode = curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    check_CURLcode();
    curlcode = curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    check_CURLcode();
    curlcode = curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    check_CURLcode();
    curlcode = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    check_CURLcode();
    curlcode = curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    check_CURLcode();

    curl_download = curl_easy_duphandle(curl);
    if (unlikely(curl_download == 0)) {
      curl_easy_cleanup(curl);
      curl_global_cleanup();
      fprintf(stderr, "Error: %u\n", __LINE__);
      return 1;
    }
    {
      memcpy(youtube_favicon, pfp_path, pfp_path_len);
      memcpy(youtube_favicon + pfp_path_len, YOUTUBE_FAVICON, sizeof(YOUTUBE_FAVICON));
      {
        i32 ret = access(youtube_favicon, R_OK);
        if (unlikely(ret == -1)) {
          if (likely(errno == ENOENT)) {
            curlcode = curl_easy_setopt(curl_download, CURLOPT_URL, YOUTUBE_FAVICON_URL);
            check_CURLcode();
            FILE *f = fopen(youtube_favicon, "wb");
            if (unlikely(f == 0)) {
              printf("%s\n", youtube_favicon);
              perror("Failed to write file");
              return 1;
            }
            curlcode = curl_easy_setopt(curl_download, CURLOPT_WRITEDATA, f);
            check_CURLcode();
            curlcode = curl_easy_perform(curl_download);
            check_CURLcode();
            fclose(f);
          } else {
            perror("Failed to access file in ./pfp/");
            return 1;
          }
        }
      }
      memcpy(twitch_favicon, pfp_path, pfp_path_len);
      memcpy(twitch_favicon + pfp_path_len, TWITCH_FAVICON, sizeof(TWITCH_FAVICON));
      {
        i32 ret = access(twitch_favicon, R_OK);
        if (unlikely(ret == -1)) {
          if (likely(errno == ENOENT)) {
            curlcode = curl_easy_setopt(curl_download, CURLOPT_URL, TWITCH_FAVICON_URL);
            check_CURLcode();
            FILE *f = fopen(twitch_favicon, "wb");
            if (unlikely(f == 0)) {
              printf("%s\n", twitch_favicon);
              perror("Failed to write file");
              return 1;
            }
            curlcode = curl_easy_setopt(curl_download, CURLOPT_WRITEDATA, f);
            check_CURLcode();
            curlcode = curl_easy_perform(curl_download);
            check_CURLcode();
            fclose(f);
          } else {
            perror("Failed to access file in ./pfp/");
            return 1;
          }
        }
      }
    }

    url = curl_url();
    curlucode = curl_url_set(url, CURLUPART_URL, "https://holodex.net/api/v2/users/live", 0);
    check_CURLUcode();
    curlucode = curl_url_set(url, CURLUPART_QUERY, config.query_str, 0);
    check_CURLUcode();
    curlcode = curl_easy_setopt(curl, CURLOPT_CURLU, url);
    check_CURLcode();

    headers = curl_slist_append(NULL, x_api);
    if (unlikely(!headers)) {
      curl_url_cleanup(url);
      curl_easy_cleanup(curl);
      curl_global_cleanup();
      return 1;
    }
    curlcode = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    check_CURLcode();

    curlcode = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, (curl_write_callback)write_callback);
    check_CURLcode();

    data.buf = push_align(arena, API_DATA_SIZE);
    data.size = 0;
    curlcode = curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    check_CURLcode();
  }

  struct notification *notify_list;
  sd_bus *bus = 0;
  {
    notify_list = push_align(arena, NUM_NOTIFICATION_MAX * sizeof(struct notification));
    memset(notify_list, 0, NUM_NOTIFICATION_MAX * sizeof(struct notification));
    i32 ret;
    ret = sd_bus_default_user(&bus);
    expect(ret >= 0);
    ret = sd_bus_match_signal(bus, NULL, NULL, "/org/freedesktop/Notifications",
                              "org.freedesktop.Notifications", "ActionInvoked", action_invoked,
                              notify_list);
    expect(ret >= 0);
    ret = sd_bus_match_signal(bus, NULL, NULL, "/org/freedesktop/Notifications",
                              "org.freedesktop.Notifications", "NotificationClosed",
                              notification_closed, notify_list);
    expect(ret >= 0);
  }

  {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
  }

  i32 epollfd;
  i32 sd_bus_fd;
  {
    epollfd = epoll_create(1);
    if (unlikely(epollfd == -1)) {
      perror("Failed to create epoll instance");
      return 1;
    }
    struct epoll_event event = {.events = EPOLLIN, .data.u32 = TIMER_RETURN};
    i32 ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, tfd, &event);
    expect(ret >= 0);
    sd_bus_fd = sd_bus_get_fd(bus);
    expect(sd_bus_fd >= 0);
    event.events = 0, event.data.u32 = SD_BUS_RETURN;
    ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, sd_bus_fd, &event);
    expect(ret >= 0);
  }

  struct live_status_ptr *intermediate =
      push_align(arena, NUM_LIVE_MAX * sizeof(struct live_status_ptr));
  void *json_buf = push_align(arena, JSON_BUF_MAX);

  struct arena arena_0 = {0};
  struct arena arena_1 = {0};
  struct arena *current_arena = &arena_0;
  {
    u64 midpoint = ((u8 *)arena->max - (u8 *)arena->current) / 2;
    arena_0.mem = arena_0.current = arena->current;
    arena_0.max = (u8 *)arena->current + midpoint - 1;
    arena_1.mem = arena_1.current = (u8 *)arena->current + midpoint;
    arena_1.max = (u8 *)arena->max;
    arena->max = arena->current;
  }

  u64 timer_out;
  struct live_status *current_status = 0;
  u32 num_current_status = 0;
  struct live_status *previous_status = 0;
  u32 num_previous_status = 0;

  while (!quit) {

    {
      i32 poll_flags = sd_bus_get_events(bus);
      i32 epoll_flags = 0;
      if (poll_flags & POLLIN) {
        epoll_flags |= EPOLLIN;
      }
      if (poll_flags & POLLOUT) {
        epoll_flags |= EPOLLOUT;
      }
      struct epoll_event event = {.events = epoll_flags, .data.u32 = SD_BUS_RETURN};
      i32 ret = epoll_ctl(epollfd, EPOLL_CTL_MOD, sd_bus_fd, &event);
      expect(ret >= 0);
    }

    struct epoll_event events[2] = {{0}, {0}};
    {
      i32 ret = epoll_wait(epollfd, events, 2, 100);
      expect(ret >= 0 || errno == EINTR);
    }

    while (sd_bus_process(bus, NULL) != 0) {
    }

    if (events[0].data.u32 == TIMER_RETURN || events[1].data.u32 == TIMER_RETURN) {
      {
        u64 r = read(tfd, &timer_out, 8);
        if (r != 8) {
          return 1;
        }
      }

      {
        struct stat st = {};
        i32 ret = fstat(config_fd, &st);
        check_ret_syscall("Failed to get config.bin");
        expect(st.st_size <= CHANNELS_FILE_MAX);
        u64 new_time = st.st_mtime;
        if (unlikely(new_time != last_mtime)) {
          i32 ret = lseek(config_fd, 0, SEEK_SET);
          check_ret_syscall("Failed seek on config.bin");
          ret = read(config_fd, config_file, st.st_size);
          check_ret_syscall("Failed to read channels file");
          expect(ret == st.st_size);
          last_mtime = new_time;
          config = parse_config(config_file, st.st_size);
          curlucode = curl_url_set(url, CURLUPART_QUERY, config.query_str, 0);
          check_CURLUcode();
        }
      }

      {
        data.size = 0;
        memset(data.buf, 0, API_DATA_SIZE);
        curlcode = curl_easy_perform(curl);
        check_CURLcode();
      }

      // current_status and num_current_status are not valid before this
      {
        reset_arena(current_arena);
        memset(json_buf, 0, JSON_BUF_MAX);
        memset(intermediate, 0, NUM_LIVE_MAX * sizeof(struct live_status_ptr));
        current_status = parse_json(current_arena, pfp_path, pfp_path_len, &data, json_buf,
                                    intermediate, &num_current_status);
        if (unlikely(current_status == 0)) {
          return 1;
        }
      }

      for (u32 i = 0; i < num_current_status; ++i) {
        i32 ret = access((c8 *)current_status[i].photo_path, R_OK);
        if (unlikely(ret == -1)) {
          if (likely(errno == ENOENT)) {
            // download it
            curlcode = curl_easy_setopt(curl_download, CURLOPT_URL, current_status[i].photo_url);
            check_CURLcode();
            FILE *f = fopen((c8 *)current_status[i].photo_path, "wb");
            if (unlikely(f == 0)) {
              printf("%s\n", current_status[i].photo_path);
              perror("Failed to write file");
              return 1;
            }
            curlcode = curl_easy_setopt(curl_download, CURLOPT_WRITEDATA, f);
            check_CURLcode();
            curlcode = curl_easy_perform(curl_download);
            check_CURLcode();
            fclose(f);
          } else {
            perror("Failed to access file in ./pfp/");
            return 1;
          }
        }
      }

      struct live_status *new[NUM_LIVE_MAX];
      u32 num_new = 0;
      {
        for (u32 i = 0; i < num_current_status; ++i) {
          struct live_status *cur = current_status + i;
          u32 found = 0;
          for (u32 j = 0; j < num_previous_status; ++j) {
            struct live_status *prev = previous_status + j;
            if (cur->id_len == prev->id_len && memcmp(cur->id, prev->id, cur->id_len) == 0) {
              found = 1;
            }
          }
          if (unlikely(!found)) {
            new[num_new++] = cur;
          }
        }
      }

      printf("Num new: %u\n", num_new);
      for (u32 i = 0; i < num_new; ++i) {
        struct live_status *cur = new[i];
        struct config_file_entry *current_config =
            bsearch_by_id(&config, cur->channel_id, cur->channel_id_len);
        expect(current_config != 0);
        i32 open_direct = current_config->open_direct;
        if (current_config->open_direct == -1) {
          open_direct = config.defaults->open_direct;
        }
        if (open_direct != 1) {
          i32 duration = current_config->duration;
          if (current_config->duration == -1) {
            duration = config.defaults->duration;
          }
          struct offset_string sound = current_config->sound;
          if (config.string_pool[sound.offset] == 0) {
            sound = config.defaults->sound;
          }
          c8 *sound_type;
          if (config.string_pool[sound.offset] != '/') {
            sound_type = "sound-name";
          } else {
            sound_type = "sound-file";
          }
          c8 *favicon = cur->link[0] == 0 ? youtube_favicon : twitch_favicon;
          c8 image_path[PATH_MAX] = "file://";
          memcpy(image_path + 7, cur->photo_path, cur->photo_path_len);
          sd_bus_message *msg = 0;
          i32 ret = sd_bus_call_method(
              bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
              "org.freedesktop.Notifications", "Notify", 0, &msg, "susssasa{sv}i", "live-notify", 0,
              favicon, config.string_pool + current_config->name.offset, cur->title, 2, "default",
              "default", 4, "category", "s", "im.received", "image-path", "s", image_path,
              sound_type, "s", config.string_pool + sound.offset, "urgency", "y", 1, duration);
          expect(ret >= 0);
          u32 id = 0;
          sd_bus_message_read_basic(msg, 'u', &id);
          sd_bus_message_unref(msg);
          i32 min_idx = 0;
          for (u32 i = 0; i < NUM_NOTIFICATION_MAX; ++i) {
            if (notify_list[i].id == 0) {
              notify_list[i].id = id;
              if (cur->link[0] == 0) {
                memcpy(notify_list[i].link, YOUTUBE_LINK, sizeof(YOUTUBE_LINK) - 1);
                expect(cur->id_len + sizeof(YOUTUBE_LINK) <= LINK_MAX_LEN);
                memcpy(notify_list[i].link + sizeof(YOUTUBE_LINK) - 1, cur->id, cur->id_len);
              } else {
                expect(cur->link_len <= LINK_MAX_LEN);
                memcpy(notify_list[i].link, cur->link, cur->link_len);
              }
              min_idx = -1;
              break;
            } else if (notify_list[i].id < notify_list[min_idx].id) {
              min_idx = i;
            }
          }
          if (min_idx != -1) {
            notify_list[min_idx].id = id;
            if (cur->link[0] == 0) {
              memcpy(notify_list[min_idx].link, YOUTUBE_LINK, sizeof(YOUTUBE_LINK) - 1);
              expect(cur->id_len + sizeof(YOUTUBE_LINK) <= LINK_MAX_LEN);
              memcpy(notify_list[min_idx].link + sizeof(YOUTUBE_LINK), cur->id, cur->id_len);
            } else {
              expect(cur->link_len <= LINK_MAX_LEN);
              memcpy(notify_list[min_idx].link, cur->link, cur->link_len);
            }
          }
        } else {
          c8 link[LINK_MAX_LEN];
          if (cur->link[0] == 0) {
            memcpy(link, YOUTUBE_LINK, sizeof(YOUTUBE_LINK) - 1);
            expect(cur->id_len + sizeof(YOUTUBE_LINK) <= LINK_MAX_LEN);
            memcpy(link + sizeof(YOUTUBE_LINK) - 1, cur->id, cur->id_len);
          } else {
            expect(cur->link_len <= LINK_MAX_LEN);
            memcpy(link, cur->link, cur->link_len);
          }
          u64 pid = fork();
          if (pid == 0) {
            i32 fd = open("/dev/null", O_WRONLY);
            if (likely(fd != -1)) {
              dup2(fd, STDOUT_FILENO);
            }
            c8 *argv0 = "/usr/bin/brave";
            c8 *argv[] = {argv0, link, 0};
            execv(argv0, argv);
          }
        }
      }

      if (current_arena == &arena_0) {
        current_arena = &arena_1;
      } else {
        current_arena = &arena_0;
      }
      previous_status = current_status;
      num_previous_status = num_current_status;
    }
  }

  close(config_fd);
  sd_bus_flush_close_unref(bus);
  curl_slist_free_all(headers);
  curl_url_cleanup(url);
  curl_easy_cleanup(curl);
  curl_easy_cleanup(curl_download);
  curl_global_cleanup();
  return 0;
}
