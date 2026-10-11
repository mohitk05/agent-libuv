/* Shell processes, threadpool file I/O and timers on the libuv loop. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

#define SHELL_OUTPUT_CAP (1024 * 1024)

void buf_append(buf_t *b, const char *p, size_t n) {
  b->data = realloc(b->data, b->len + n + 1);
  memcpy(b->data + b->len, p, n);
  b->len += n;
  b->data[b->len] = 0;
}

/* ---------- shell ---------- */

typedef struct {
  shell_cb cb;
  void *ud;
  uv_process_t proc;
  uv_pipe_t pipe;
  uv_timer_t timer;
  int open_handles;
  int timed_out;
  int64_t status;
  buf_t out;
} shell_t;

static void shell_close_cb(uv_handle_t *h) {
  shell_t *s = h->data;
  if (--s->open_handles > 0) return;
  s->cb(s->ud, s->status, s->timed_out, s->out.data ? s->out.data : "", s->out.len);
  free(s->out.data);
  free(s);
}

static void shell_close(uv_handle_t *h) {
  if (!uv_is_closing(h)) uv_close(h, shell_close_cb);
}

static void shell_alloc_cb(uv_handle_t *h, size_t sz, uv_buf_t *b) {
  b->base = malloc(sz);
  b->len = sz;
}

static void shell_read_cb(uv_stream_t *st, ssize_t n, const uv_buf_t *b) {
  shell_t *s = st->data;
  if (n > 0 && s->out.len < SHELL_OUTPUT_CAP) buf_append(&s->out, b->base, (size_t)n);
  free(b->base);
  if (n < 0) shell_close((uv_handle_t *)st);
}

static void shell_exit_cb(uv_process_t *p, int64_t status, int sig) {
  shell_t *s = p->data;
  s->status = status;
  shell_close((uv_handle_t *)&s->timer);
  shell_close((uv_handle_t *)p);
}

static void shell_timeout_cb(uv_timer_t *t) {
  shell_t *s = t->data;
  s->timed_out = 1;
  uv_process_kill(&s->proc, SIGKILL);
}

void shell_run(const char *cmd, int timeout_ms, void *ud, shell_cb cb) {
  shell_t *s = calloc(1, sizeof *s);
  s->cb = cb;
  s->ud = ud;
  s->open_handles = 3; /* process, pipe, timer */

  size_t n = strlen(cmd) + 16;
  char *full = malloc(n);
  snprintf(full, n, "(%s) 2>&1", cmd);
#ifdef _WIN32
  char *argv[] = {"cmd.exe", "/c", full, NULL};
#else
  char *argv[] = {"/bin/sh", "-c", full, NULL};
#endif

  uv_pipe_init(loop, &s->pipe, 0);
  uv_timer_init(loop, &s->timer);
  s->pipe.data = s->proc.data = s->timer.data = s;

  uv_stdio_container_t stdio[3];
  stdio[0].flags = UV_IGNORE;
  stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
  stdio[1].data.stream = (uv_stream_t *)&s->pipe;
  stdio[2].flags = UV_IGNORE;

  uv_process_options_t opts = {0};
  opts.file = argv[0];
  opts.args = argv;
  opts.stdio_count = 3;
  opts.stdio = stdio;
  opts.exit_cb = shell_exit_cb;

  int r = uv_spawn(loop, &s->proc, &opts);
  free(full);
  if (r) {
    const char *e = uv_strerror(r);
    s->status = -1;
    buf_append(&s->out, "spawn failed: ", 14);
    buf_append(&s->out, e, strlen(e));
    s->open_handles = 2; /* the process handle was never initialised */
    uv_close((uv_handle_t *)&s->pipe, shell_close_cb);
    uv_close((uv_handle_t *)&s->timer, shell_close_cb);
    return;
  }
  uv_read_start((uv_stream_t *)&s->pipe, shell_alloc_cb, shell_read_cb);
  if (timeout_ms > 0) uv_timer_start(&s->timer, shell_timeout_cb, (uint64_t)timeout_ms, 0);
}

/* ---------- file I/O on the threadpool ---------- */

typedef struct {
  uv_work_t req;
  int write;
  char *path;
  char *data;
  size_t len, max;
  char err[160];
  void *ud;
  fs_cb cb;
} fs_op_t;

static void fs_work(uv_work_t *w) {
  fs_op_t *op = w->data;
  FILE *f = fopen(op->path, op->write ? "wb" : "rb");
  if (!f) {
    snprintf(op->err, sizeof op->err, "%s: %s", op->path, strerror(errno));
    return;
  }
  if (op->write) {
    if (fwrite(op->data, 1, op->len, f) != op->len)
      snprintf(op->err, sizeof op->err, "%s: write failed", op->path);
  } else {
    buf_t b = {0};
    char chunk[4096];
    size_t n;
    while ((op->max == 0 || b.len < op->max) && (n = fread(chunk, 1, sizeof chunk, f)) > 0)
      buf_append(&b, chunk, n);
    if (op->max && b.len > op->max) b.data[b.len = op->max] = 0;
    free(op->data);
    op->data = b.data;
    op->len = b.len;
  }
  fclose(f);
}

static void fs_after(uv_work_t *w, int status) {
  fs_op_t *op = w->data;
  op->cb(op->ud, op->err[0] ? op->err : NULL, op->data ? op->data : "", op->len);
  free(op->path);
  free(op->data);
  free(op);
}

static void fs_start(fs_op_t *op) {
  op->req.data = op;
  uv_queue_work(loop, &op->req, fs_work, fs_after);
}

void fs_read_async(const char *path, size_t max_bytes, void *ud, fs_cb cb) {
  fs_op_t *op = calloc(1, sizeof *op);
  op->path = strdup(path);
  op->max = max_bytes;
  op->ud = ud;
  op->cb = cb;
  fs_start(op);
}

void fs_write_async(const char *path, const char *data, size_t len, void *ud, fs_cb cb) {
  fs_op_t *op = calloc(1, sizeof *op);
  op->write = 1;
  op->path = strdup(path);
  op->data = malloc(len + 1);
  memcpy(op->data, data, len);
  op->len = len;
  op->ud = ud;
  op->cb = cb;
  fs_start(op);
}

/* ---------- timers ---------- */

typedef struct {
  uv_timer_t t;
  void *ud;
  void (*cb)(void *);
} after_t;

static void after_close_cb(uv_handle_t *h) { free(h->data); }

static void after_fire(uv_timer_t *t) {
  after_t *a = t->data;
  void *ud = a->ud;
  void (*cb)(void *) = a->cb;
  uv_close((uv_handle_t *)t, after_close_cb);
  cb(ud);
}

void timer_after(uint64_t ms, void *ud, void (*cb)(void *)) {
  after_t *a = calloc(1, sizeof *a);
  a->ud = ud;
  a->cb = cb;
  uv_timer_init(loop, &a->t);
  a->t.data = a;
  uv_timer_start(&a->t, after_fire, ms, 0);
}
