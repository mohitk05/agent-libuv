/* Async I/O primitives shared by the agent core and the script bridge.
 * Everything here completes on the libuv loop thread. */
#ifndef RUNTIME_H
#define RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <uv.h>

extern uv_loop_t *loop;

typedef struct {
  char *data;
  size_t len;
} buf_t;

void buf_append(buf_t *b, const char *p, size_t n);

/* HTTP via libcurl's multi-socket API, driven by uv_poll/uv_timer.
 * err is NULL on success. body is NUL-terminated and freed after cb returns. */
typedef void (*http_cb)(void *ud, const char *err, long status, const char *body, size_t len);
void http_init(void);
void http_request(const char *method, const char *url, const char **headers, int nheaders,
                  const char *body, long timeout_ms, void *ud, http_cb cb);

/* Run a shell command; stdout+stderr are merged. out is freed after cb returns. */
typedef void (*shell_cb)(void *ud, int64_t status, int timed_out, const char *out, size_t len);
void shell_run(const char *cmd, int timeout_ms, void *ud, shell_cb cb);

/* File access on the libuv threadpool. err is NULL on success. */
typedef void (*fs_cb)(void *ud, const char *err, const char *data, size_t len);
void fs_read_async(const char *path, size_t max_bytes, void *ud, fs_cb cb);
void fs_write_async(const char *path, const char *data, size_t len, void *ud, fs_cb cb);

void timer_after(uint64_t ms, void *ud, void (*cb)(void *ud));

#endif
