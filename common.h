#ifndef COMMON_H_
#define COMMON_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define u8 uint8_t
#define c8 char
#define u32 uint32_t
#define i32 int32_t
#define u64 uint64_t
#define i64 int64_t
#define internal static

#define API_DATA_SIZE (8 * 1024 * 1024)
#define LINK_MAX_LEN (128 - 4)
#define ALLOC_MEM (64 * 1024 * 1024)
#define CHANNELS_FILE_MAX (1024 * 1024)
#define JSON_BUF_MAX (2 * 1024 * 1024)
#define NUM_LIVE_MAX 512
#define NUM_NOTIFICATION_MAX (NUM_LIVE_MAX * 2)
#define PFP_REL "/pfp/"
#define DELAY_MINUTE 3
#define YOUTUBE_FAVICON_URL                                                                        \
  "https://www.gstatic.com/youtube/img/branding/favicon/"                                          \
  "favicon_144x144_v2.png"
#define YOUTUBE_FAVICON "youtube.png"
#define TWITCH_FAVICON_URL "https://static.twitchcdn.net/assets/favicon-32-e29e246c157142c94346.png"
#define TWITCH_FAVICON "twitch.png"
#define YOUTUBE_LINK "https://www.youtube.com/watch?v="
#define TIMER_RETURN 1
#define SD_BUS_RETURN 2

#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

#define expect(expr)                                                                               \
  do {                                                                                             \
    if (unlikely(!(expr))) {                                                                       \
      fprintf(stderr, "Error at %s: %u\n", __FILE__, __LINE__);                                    \
      exit(1);                                                                                     \
    }                                                                                              \
  } while (0)

#define expect_errno(expr, msg)                                                                    \
  do {                                                                                             \
    if (unlikely(!(expr))) {                                                                       \
      perror(msg);                                                                                 \
      exit(1);                                                                                     \
    }                                                                                              \
  } while (0)

struct __attribute__((__packed__)) offset_string {
  u32 offset;
  u32 len;
};

struct arena {
  void *mem;
  void *current;
  void *max;
};

internal inline void *push_align(struct arena *arena, u64 size) {
  void *ret =
      (void *)(((u64)arena->current + (_Alignof(max_align_t) - 1)) & ~(_Alignof(max_align_t) - 1));
  arena->current = (u8 *)ret + size;
  if (unlikely(arena->current > arena->max)) {
    exit(1);
  }
  return ret;
}

internal inline void *push(struct arena *arena, u64 size) {
  void *ret = arena->current;
  arena->current = (u8 *)ret + size;
  if (unlikely(arena->current > arena->max)) {
    exit(1);
  }
  return ret;
}

// void *set_point(struct arena *arena) { return arena->current; }
//
// void restore_point(struct arena *arena, void *point) { arena->current =
// point; }

void reset_arena(struct arena *arena) {
  arena->current = arena->mem;
}

#endif
