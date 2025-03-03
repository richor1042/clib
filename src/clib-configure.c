//
// clib-configure.c
//
// Copyright (c) 2012-2021 clib authors
// MIT licensed
//

#include <curl/curl.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <unistd.h>
#endif

#ifdef HAVE_PTHREADS
#include <pthread.h>
#endif

#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

#include "common/clib-cache.h"
#include "common/clib-package.h"
#include "common/clib-settings.h"

#include <asprintf/asprintf.h>
#include <commander/commander.h>
#include <debug/debug.h>
#include <fs/fs.h>
#include <hash/hash.h>
#include <list/list.h>
#include <logger/logger.h>
#include <path-join/path-join.h>
#include <str-flatten/str-flatten.h>
#include <trim/trim.h>

#include "version.h"

#define PROGRAM_NAME "clib-configure"

#define SX(s) #s
#define S(s) SX(s)

#if defined(_WIN32) || defined(WIN32) || defined(__MINGW32__) ||               \
    defined(__MINGW64__)
#define setenv(k, v, _) _putenv_s(k, v)
#define realpath(a, b) _fullpath(a, b, strlen(a))
#endif

typedef struct options options_t;
struct options {
  const char *dir;
  char *prefix;
  int force;
  int verbose;
  int dev;
  int skip_cache;
  int flags;
  int global;
#ifdef HAVE_PTHREADS
  unsigned int concurrency;
#endif
};

clib_package_opts_t package_opts = {0};
clib_package_t *root_package = 0;

hash_t *configured = 0;
command_t program = {0};
debug_t debugger = {0};

char **rest_argv = 0;
int rest_offset = 0;
int rest_argc = 0;

options_t opts = {.skip_cache = 0,
                  .verbose = 1,
                  .force = 0,
                  .dev = 0,
#ifdef HAVE_PTHREADS
                  .concurrency = MAX_THREADS,
#endif

#ifdef _WIN32
                  .dir = ".\\deps"
#else
                  .dir = "./deps"
#endif

};

int configure_package(const char *dir);

#ifdef HAVE_PTHREADS
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
typedef struct clib_package_thread clib_package_thread_t;
struct clib_package_thread {
  const char *dir;
};

void *configure_package_with_manifest_name_thread(void *arg) {
  clib_package_thread_t *wrap = arg;
  const char *dir = wrap->dir;
  configure_package(dir);
  return 0;
}
#endif

int configure_package_with_manifest_name(const char *dir, const char *file) {
  clib_package_t *package = 0;
  char *json = NULL;
  int ok = 0;
  int rc = 0;

#ifdef PATH_MAX
  long path_max = PATH_MAX;
#elif defined(_PC_PATH_MAX)
  long path_max = pathconf(dir, _PC_PATH_MAX);
#else
  long path_max = 4096;
#endif

  char *path = path_join(dir, file);

  if (0 == path) {
    return -ENOMEM;
  }

#ifdef HAVE_PTHREADS
  pthread_mutex_lock(&mutex);
#endif

  if (!root_package) {
    const char *name = NULL;
    unsigned int i = 0;

    do {
      name = manifest_names[i];
      json = fs_read(name);
    } while (NULL != manifest_names[++i] && !json);

    if (json) {
      root_package = clib_package_new(json, opts.verbose);
    }

    if (root_package && root_package->prefix) {
      char prefix[path_max];
      memset(prefix, 0, path_max);
      realpath(root_package->prefix, prefix);
      unsigned long int size = strlen(prefix) + 1;
      free(root_package->prefix);
      root_package->prefix = malloc(size);
      memset((void *)root_package->prefix, 0, size);
      memcpy((void *)root_package->prefix, prefix, size);
    }
  }

  if (hash_has(configured, path)) {
#ifdef HAVE_PTHREADS
    pthread_mutex_unlock(&mutex);
#endif
    goto cleanup;
  }

#ifdef HAVE_PTHREADS
  pthread_mutex_unlock(&mutex);
#endif

  // Free the json if it was allocated before attempting to modify it
  free(json);

  if (0 == fs_exists(path)) {
    debug(&debugger, "read %s", path);
    json = fs_read(path);
  }

  if (0 != json) {
#ifdef DEBUG
    package = clib_package_new(json, 1);
#else
    package = clib_package_new(json, 0);
#endif
  } else {
#ifdef DEBUG
    package = clib_package_new_from_slug(dir, 1);
#else
    package = clib_package_new_from_slug(dir, 0);
#endif
  }

  if (0 == package) {
    rc = -ENOMEM;
    goto cleanup;
  }

  if (0 != package->flags && opts.flags) {
#ifdef HAVE_PTHREADS
    rc = pthread_mutex_lock(&mutex);
#endif

    hash_set(configured, path, "t");
    ok = 1;
    fprintf(stdout, "%s ", trim(package->flags));
    fflush(stdout);
  } else if (0 != package->configure) {
    char *command = 0;
    char *args = rest_argc > 0
                     ? str_flatten((const char **)rest_argv, 0, rest_argc)
                     : "";

    asprintf(&command, "cd %s && %s %s", dir, package->configure, args);

    if (root_package && root_package->prefix) {
      package_opts.prefix = root_package->prefix;
      clib_package_set_opts(package_opts);
      setenv("PREFIX", package_opts.prefix, 1);
    } else if (opts.prefix) {
      setenv("PREFIX", opts.prefix, 1);
    } else if (package->prefix) {
      char prefix[path_max];
      memset(prefix, 0, path_max);
      realpath(package->prefix, prefix);
      unsigned long int size = strlen(prefix) + 1;
      free(package->prefix);
      package->prefix = malloc(size);
      memset((void *)package->prefix, 0, size);
      memcpy((void *)package->prefix, prefix, size);
      setenv("PREFIX", package->prefix, 1);
    }

    if (rest_argc > 0) {
      free(args);
    }

    if (0 != opts.verbose) {
      logger_warn("configure", "%s: %s", package->name, package->configure);
    }

    debug(&debugger, "system: %s", command);
    rc = system(command);
    free(command);
    command = 0;
#ifdef HAVE_PTHREADS
    rc = pthread_mutex_lock(&mutex);
#endif

    hash_set(configured, path, "t");
    ok = 1;
  } else {
#ifdef HAVE_PTHREADS
    rc = pthread_mutex_lock(&mutex);
#endif

    hash_set(configured, path, "f");
    ok = 1;
  }

  if (0 != rc) {
    goto cleanup;
  }

#ifdef HAVE_PTHREADS
  pthread_mutex_unlock(&mutex);
#endif

  if (0 != package->dependencies) {
    list_iterator_t *iterator = 0;
    list_node_t *node = 0;

#ifdef HAVE_PTHREADS
    clib_package_thread_t wraps[opts.concurrency];
    pthread_t threads[opts.concurrency];
    unsigned int i = 0;
#endif

    iterator = list_iterator_new(package->dependencies, LIST_HEAD);

    while ((node = list_iterator_next(iterator))) {
      clib_package_dependency_t *dep = node->val;
      char *slug = 0;
      asprintf(&slug, "%s/%s@%s", dep->author, dep->name, dep->version);

      clib_package_t *dependency = clib_package_new_from_slug(slug, 0);
      char *dep_dir = path_join(opts.dir, dependency->name);

      free(slug);
      clib_package_free(dependency);

#ifdef HAVE_PTHREADS
      clib_package_thread_t *wrap = &wraps[i];
      pthread_t *thread = &threads[i];
      wrap->dir = dep_dir;
      rc = pthread_create(thread, 0,
                          configure_package_with_manifest_name_thread, wrap);

      if (++i >= opts.concurrency) {
        for (int j = 0; j < i; ++j) {
          pthread_join(threads[j], 0);
          free((void *)wraps[j].dir);
        }

        i = 0;
      }
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
      if (!opts.flags) {
        usleep(1024 * 10);
      }
#endif
#else
      if (0 == dep_dir) {
        rc = -ENOMEM;
        goto cleanup;
      }

      rc = configure_package(dep_dir);

      free((void *)dep_dir);

      if (0 != rc) {
        goto cleanup;
      }
#endif
    }

#ifdef HAVE_PTHREADS
    for (int j = 0; j < i; ++j) {
      pthread_join(threads[j], 0);
      free((void *)wraps[j].dir);
    }
#endif

    if (0 != iterator) {
      list_iterator_destroy(iterator);
    }
  }

  if (opts.dev && 0 != package->development) {
    list_iterator_t *iterator = 0;
    list_node_t *node = 0;

#ifdef HAVE_PTHREADS
    clib_package_thread_t wraps[opts.concurrency];
    pthread_t threads[opts.concurrency];
    unsigned int i = 0;
#endif

    iterator = list_iterator_new(package->development, LIST_HEAD);

    while ((node = list_iterator_next(iterator))) {
      clib_package_dependency_t *dep = node->val;
      char *slug = 0;
      asprintf(&slug, "%s/%s@%s", dep->author, dep->name, dep->version);

      clib_package_t *dependency = clib_package_new_from_slug(slug, 0);
      char *dep_dir = path_join(opts.dir, dependency->name);

      free(slug);
      clib_package_free(dependency);

#ifdef HAVE_PTHREADS
      clib_package_thread_t *wrap = &wraps[i];
      pthread_t *thread = &threads[i];
      wrap->dir = dep_dir;
      rc = pthread_create(thread, 0,
                          configure_package_with_manifest_name_thread, wrap);

      if (++i >= opts.concurrency) {
        for (int j = 0; j < i; ++j) {
          pthread_join(threads[j], 0);
          free((void *)wraps[j].dir);
        }

        i = 0;
      }
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
      if (!opts.flags) {
        usleep(1024 * 10);
      }
#endif
#else
      if (0 == dep_dir) {
        rc = -ENOMEM;
        goto cleanup;
      }

      rc = configure_package(dep_dir);

      free((void *)dep_dir);

      if (0 != rc) {
        goto cleanup;
      }
#endif
    }

#ifdef HAVE_PTHREADS
    for (int j = 0; j < i; ++j) {
      pthread_join(threads[j], 0);
      free((void *)wraps[j].dir);
    }
#endif

    if (0 != iterator) {
      list_iterator_destroy(iterator);
    }
  }

cleanup:
  if (0 != package) {
    clib_package_free(package);
  }
  if (0 != json) {
    free(json);
  }
  if (0 == ok) {
    if (0 != path) {
      free(path);
    }
  }
  return rc;
}

int configure_package(const char *dir) {
  const char *name = NULL;
  unsigned int i = 0;
  int rc = 0;

  do {
    name = manifest_names[i];
    rc = configure_package_with_manifest_name(dir, name);
  } while (NULL != manifest_names[++i] && 0 != rc);

  return rc;
}

static void setopt_skip_cache(command_t *self) {
  opts.skip_cache = 1;
  debug(&debugger, "set skip cache flag");
}

static void setopt_dev(command_t *self) {
  opts.dev = 1;
  debug(&debugger, "set dev flag");
}

static void setopt_force(command_t *self) {
  opts.force = 1;
  debug(&debugger, "set force flag");
}

static void setopt_global(command_t *self) {
  opts.global = 1;
  debug(&debugger, "set global flag");
}

static void setopt_flags(command_t *self) {
  opts.flags = 1;
  opts.verbose = 0;
  debug(&debugger, "set flags flag");
}

static void setopt_prefix(command_t *self) {
  opts.prefix = (char *)self->arg;
  debug(&debugger, "set prefix: %s", opts.prefix);
}

static void setopt_dir(command_t *self) {
  opts.dir = (char *)self->arg;
  debug(&debugger, "set dir: %s", opts.dir);
}

static void setopt_quiet(command_t *self) {
  opts.verbose = 0;
  debug(&debugger, "set quiet flag");
}

#ifdef HAVE_PTHREADS
static void setopt_concurrency(command_t *self) {
  if (self->arg) {
    opts.concurrency = atol(self->arg);
    debug(&debugger, "set concurrency: %lu", opts.concurrency);
  }
}
#endif

int main(int argc, char **argv) {
  int rc = 0;

#ifdef PATH_MAX
  long path_max = PATH_MAX;
#elif defined(_PC_PATH_MAX)
  long path_max = pathconf(opts.dir, _PC_PATH_MAX);
#else
  long path_max = 4096;
#endif

  char CWD[path_max];

  memset(CWD, 0, path_max);

  if (0 == getcwd(CWD, path_max)) {
    return -errno;
  }

  configured = hash_new();
  hash_set(configured, strdup("__" PROGRAM_NAME "__"), CLIB_VERSION);

  command_init(&program, PROGRAM_NAME, CLIB_VERSION);
  debug_init(&debugger, PROGRAM_NAME);

  program.usage = "[options] [name ...]";

  command_option(&program, "-o", "--out <dir>",
                 "change the output directory [deps]", setopt_dir);

  command_option(&program, "-P", "--prefix <dir>",
                 "change the prefix directory (usually '/usr/local')",
                 setopt_prefix);

  command_option(&program, "-q", "--quiet", "disable verbose output",
                 setopt_quiet);

  command_option(&program, "-d", "--dev", "configure development dependencies",
                 setopt_dev);

  command_option(&program, "-f", "--force",
                 "force the action of something, like overwriting a file",
                 setopt_force);

  command_option(&program, "--cflags", "--flags",
                 "output compiler flags instead of configuring", setopt_flags);

  command_option(&program, "-c", "--skip-cache", "skip cache when configuring",
                 setopt_skip_cache);

#ifdef HAVE_PTHREADS
  command_option(&program, "-C", "--concurrency <number>",
                 "Set concurrency (default: " S(MAX_THREADS) ")",
                 setopt_concurrency);
#endif

  command_parse(&program, argc, argv);

  if (opts.dir) {
    char dir[path_max];
    memset(dir, 0, path_max);
    realpath(opts.dir, dir);
    unsigned long int size = strlen(dir) + 1;
    opts.dir = malloc(size);
    memset((void *)opts.dir, 0, size);
    memcpy((void *)opts.dir, dir, size);
  }

  if (opts.prefix) {
    char prefix[path_max];
    memset(prefix, 0, path_max);
    realpath(opts.prefix, prefix);
    unsigned long int size = strlen(prefix) + 1;
    opts.prefix = malloc(size);
    memset((void *)opts.prefix, 0, size);
    memcpy((void *)opts.prefix, prefix, size);
  }

  rest_offset = program.argc;

  if (argc > 0) {
    int rest = 0;
    int i = 0;
    do {
      char *arg = program.nargv[i];
      if (arg && '-' == arg[0] && '-' == arg[1] && 2 == strlen(arg)) {
        rest = 1;
        rest_offset = i + 1;
      } else if (arg && rest) {
        (void)rest_argc++;
      }
    } while (program.nargv[++i]);
  }

  if (rest_argc > 0) {
    rest_argv = malloc(rest_argc * sizeof(char *));
    memset(rest_argv, 0, rest_argc * sizeof(char *));

    int j = 0;
    int i = rest_offset;
    do {
      rest_argv[j++] = program.nargv[i++];
    } while (program.nargv[i]);
  }

  if (0 != curl_global_init(CURL_GLOBAL_ALL)) {
    logger_error("error", "Failed to initialize cURL");
    return 1;
  }

  clib_cache_init(CLIB_PACKAGE_CACHE_TIME);

  package_opts.skip_cache = opts.skip_cache;
  package_opts.prefix = opts.prefix;
  package_opts.global = opts.global;
  package_opts.force = opts.force;

  clib_package_set_opts(package_opts);

  if (0 == program.argc || (argc == rest_offset + rest_argc)) {
    rc = configure_package(CWD);
  } else {
    for (int i = 1; i <= rest_offset; ++i) {
      char *dep = program.nargv[i];

      if ('.' == dep[0]) {
        char dir[path_max];
        memset(dir, 0, path_max);
        dep = realpath(dep, dir);
      } else {
        fs_stats *stats = fs_stat(dep);
        if (!stats) {
          dep = path_join(opts.dir, dep);
        } else {
          free(stats);
        }
      }

      fs_stats *stats = fs_stat(dep);

      if (stats && (S_IFREG == (stats->st_mode & S_IFMT)
#if defined(__unix__) || defined(__linux__) || defined(_POSIX_VERSION)
                    || S_IFLNK == (stats->st_mode & S_IFMT)
#endif
                        )) {
        dep = basename(dep);
        rc = configure_package_with_manifest_name(dirname(dep), basename(dep));
      } else {
        rc = configure_package(dep);

        // try with slug
        if (0 != rc) {
          rc = configure_package(program.nargv[i]);
        }
      }

      if (stats) {
        free(stats);
        stats = 0;
      }
    }
  }

  int total_configured = 0;
  hash_each(configured, {
    if (0 == strncmp("t", val, 1)) {
      (void)total_configured++;
    }
    if (0 != key) {
      free((void *)key);
    }
  });

  hash_free(configured);
  command_free(&program);
  curl_global_cleanup();
  clib_package_cleanup();

  if (opts.dir) {
    free((void *)opts.dir);
  }

  if (opts.prefix) {
    free(opts.prefix);
  }

  if (rest_argc > 0) {
    free(rest_argv);
    rest_offset = 0;
    rest_argc = 0;
    rest_argv = 0;
  }

  if (0 == rc) {
    if (opts.flags && total_configured > 0) {
      printf("\n");
    }

    if (opts.verbose) {
      if (total_configured > 1) {
        logger_info("info", "configured %d packages", total_configured);
      } else if (1 == total_configured) {
        logger_info("info", "configured 1 package");
      } else {
        logger_info("info", "configured 0 packages");
      }
    }
  }

  return rc;
}
