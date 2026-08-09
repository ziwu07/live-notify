#ifndef XDG_BASE_DIRECTORY_H_
#define XDG_BASE_DIRECTORY_H_

#include "common.h"

#define gen_xdg(name, env, def)                                                                    \
  internal inline u32 get_path_xdg_##name(c8 *buf) {                                               \
    c8 *var = getenv(#env);                                                                        \
    if (var != 0) {                                                                                \
      u32 i;                                                                                       \
      for (i = 0; i < PATH_MAX; ++i) {                                                             \
        buf[i] = var[i];                                                                           \
        if (var[i] == 0) {                                                                         \
          break;                                                                                   \
        }                                                                                          \
      }                                                                                            \
      for (u32 j = 0; j < sizeof(PROJECT_PATH_NAME) - 1; ++j) {                                    \
        buf[i++] = PROJECT_PATH_NAME[j];                                                           \
      }                                                                                            \
      return i;                                                                                    \
    } else {                                                                                       \
      c8 *home = getenv("HOME");                                                                   \
      u32 i;                                                                                       \
      for (i = 0; i < PATH_MAX; ++i) {                                                             \
        buf[i] = home[i];                                                                          \
        if (home[i] == 0) {                                                                        \
          break;                                                                                   \
        }                                                                                          \
      }                                                                                            \
      for (u32 j = 0; j < sizeof(def) - 1; ++j) {                                                  \
        buf[i++] = def[j];                                                                         \
      }                                                                                            \
      for (u32 j = 0; j < sizeof(PROJECT_PATH_NAME) - 1; ++j) {                                    \
        buf[i++] = PROJECT_PATH_NAME[j];                                                           \
      }                                                                                            \
      return i;                                                                                    \
    }                                                                                              \
  }

gen_xdg(config, XDG_CONFIG_HOME, "/.config") gen_xdg(state, XDG_STATE_HOME, "/.local/state")
    gen_xdg(cache, XDG_CACHE_HOME, "/.cache")

#define get_file_path(type, name, buf)                                                             \
  do {                                                                                             \
    u32 i = get_path_xdg_##type(buf);                                                              \
    for (u32 j = 0; j < sizeof("/" name); ++j) {                                                   \
      buf[i++] = "/" name[j];                                                                      \
    }                                                                                              \
  } while (0)

#endif
