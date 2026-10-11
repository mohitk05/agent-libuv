#include <json-c/json.h>
#include <quickjs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prelude_gen.h"
#include "runtime.h"
#include "script.h"

static JSRuntime *rt;
static JSContext *ctx;
static char *tools_json, *config_json;

/* ---------- plumbing ---------- */

static char *error_string(JSContext *c, JSValueConst e) {
  const char *s = JS_ToCString(c, e);
  JSValue st = JS_IsObject(e) ? JS_GetPropertyStr(c, e, "stack") : JS_UNDEFINED;
  const char *stack = JS_IsString(st) ? JS_ToCString(c, st) : NULL;
  size_t n = (s ? strlen(s) : 8) + (stack ? strlen(stack) : 0) + 2;
  char *out = malloc(n);
  snprintf(out, n, "%s%s%s", s ? s : "(error)", stack ? "\n" : "", stack ? stack : "");
  if (s) JS_FreeCString(c, s);
  if (stack) JS_FreeCString(c, stack);
  JS_FreeValue(c, st);
  return out;
}

static void dump_exception(JSContext *c) {
  JSValue e = JS_GetException(c);
  char *s = error_string(c, e);
  fprintf(stderr, "[script] %s\n", s);
  free(s);
  JS_FreeValue(c, e);
}

/* Promise reactions and async completions queue jobs; run them to quiescence. */
static void drain_jobs(void) {
  static int draining;
  if (draining) return; /* the outer loop will pick up anything queued meanwhile */
  draining = 1;
  for (;;) {
    JSContext *c;
    int r = JS_ExecutePendingJob(rt, &c);
    if (r == 0) break;
    if (r < 0) dump_exception(c);
  }
  draining = 0;
}

static void rejection_tracker(JSContext *c, JSValueConst promise, JSValueConst reason,
                              bool is_handled, void *opaque) {
  if (is_handled) return;
  char *s = error_string(c, reason);
  fprintf(stderr, "[script] unhandled promise rejection: %s\n", s);
  free(s);
}

static char *read_all(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  buf_t b = {0};
  char chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_append(&b, chunk, n);
  fclose(f);
  if (!b.data) b.data = calloc(1, 1);
  *len = b.len;
  return b.data;
}

static JSModuleDef *module_loader(JSContext *c, const char *name, void *opaque) {
  size_t len;
  char *src = read_all(name, &len);
  if (!src) {
    JS_ThrowReferenceError(c, "could not load module '%s'", name);
    return NULL;
  }
  JSValue v = JS_Eval(c, src, len, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  free(src);
  if (JS_IsException(v)) return NULL;
  JSModuleDef *m = JS_VALUE_GET_PTR(v);
  JS_FreeValue(c, v);
  return m;
}

/* ---------- async natives: each returns a promise settled from the loop ---------- */

typedef struct {
  JSValue resolve, reject;
} op_t;

static JSValue op_begin(JSContext *c, op_t **out) {
  JSValue fns[2];
  JSValue p = JS_NewPromiseCapability(c, fns);
  op_t *o = malloc(sizeof *o);
  o->resolve = fns[0];
  o->reject = fns[1];
  *out = o;
  return p;
}

static void op_settle(op_t *o, int ok, JSValue v) {
  JSValue r = JS_Call(ctx, ok ? o->resolve : o->reject, JS_UNDEFINED, 1, &v);
  JS_FreeValue(ctx, r);
  JS_FreeValue(ctx, v);
  JS_FreeValue(ctx, o->resolve);
  JS_FreeValue(ctx, o->reject);
  free(o);
  drain_jobs();
}

static void op_reject_msg(op_t *o, const char *msg) {
  JSValue e = JS_NewError(ctx);
  JS_SetPropertyStr(ctx, e, "message", JS_NewString(ctx, msg));
  op_settle(o, 0, e);
}

static char *arg_str(JSContext *c, JSValueConst v) {
  const char *s = JS_ToCString(c, v);
  char *out = strdup(s ? s : "");
  if (s) JS_FreeCString(c, s);
  return out;
}

static JSValue n_log(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
  int32_t level = 0;
  JS_ToInt32(c, &level, argv[0]);
  char *s = arg_str(c, argv[1]);
  fprintf(level ? stderr : stdout, "%s\n", s);
  free(s);
  return JS_UNDEFINED;
}

static JSValue n_env(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
  char *name = arg_str(c, argv[0]);
  const char *v = getenv(name);
  free(name);
  return v ? JS_NewString(c, v) : JS_UNDEFINED;
}

static void sleep_done(void *ud) { op_settle(ud, 1, JS_UNDEFINED); }

static JSValue n_sleep(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
  int32_t ms = 0;
  JS_ToInt32(c, &ms, argv[0]);
  op_t *o;
  JSValue p = op_begin(c, &o);
  timer_after(ms < 0 ? 0 : (uint64_t)ms, o, sleep_done);
  return p;
}

static void exec_done(void *ud, int64_t status, int timed_out, const char *out, size_t len) {
  JSValue r = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, r, "code", JS_NewInt64(ctx, status));
  JS_SetPropertyStr(ctx, r, "output", JS_NewStringLen(ctx, out, len));
  JS_SetPropertyStr(ctx, r, "timedOut", JS_NewBool(ctx, timed_out));
  op_settle(ud, 1, r);
}

static JSValue n_exec(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
  char *cmd = arg_str(c, argv[0]);
  int32_t timeout = 30000;
  JS_ToInt32(c, &timeout, argv[1]);
  op_t *o;
  JSValue p = op_begin(c, &o);
  shell_run(cmd, timeout, o, exec_done);
  free(cmd);
  return p;
}

static void fs_done(void *ud, const char *err, const char *data, size_t len) {
  if (err) op_reject_msg(ud, err);
  else op_settle(ud, 1, JS_NewStringLen(ctx, data, len));
}

static JSValue n_read_file(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
  char *path = arg_str(c, argv[0]);
  op_t *o;
  JSValue p = op_begin(c, &o);
  fs_read_async(path, 0, o, fs_done);
  free(path);
  return p;
}

static void write_done(void *ud, const char *err, const char *data, size_t len) {
  if (err) op_reject_msg(ud, err);
  else op_settle(ud, 1, JS_UNDEFINED);
}

static JSValue n_write_file(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
  char *path = arg_str(c, argv[0]);
  size_t len;
  const char *data = JS_ToCStringLen(c, &len, argv[1]);
  op_t *o;
  JSValue p = op_begin(c, &o);
  fs_write_async(path, data ? data : "", data ? len : 0, o, write_done);
  if (data) JS_FreeCString(c, data);
  free(path);
  return p;
}

static void fetch_done(void *ud, const char *err, long status, const char *body, size_t len) {
  if (err) {
    op_reject_msg(ud, err);
    return;
  }
  JSValue r = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, r, "status", JS_NewInt64(ctx, status));
  JS_SetPropertyStr(ctx, r, "body", JS_NewStringLen(ctx, body, len));
  op_settle(ud, 1, r);
}

/* fetch(method, url, headersJson, body, timeoutMs) */
static JSValue n_fetch(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
  char *method = arg_str(c, argv[0]), *url = arg_str(c, argv[1]);
  char *hjson = arg_str(c, argv[2]), *body = arg_str(c, argv[3]);
  int32_t timeout = 60000;
  JS_ToInt32(c, &timeout, argv[4]);

  json_object *h = json_tokener_parse(hjson);
  int n = h && json_object_is_type(h, json_type_object) ? json_object_object_length(h) : 0;
  char **lines = calloc(n + 1, sizeof *lines);
  int i = 0;
  if (n) {
    json_object_object_foreach(h, k, v) {
      size_t sz = strlen(k) + strlen(json_object_get_string(v)) + 3;
      lines[i] = malloc(sz);
      snprintf(lines[i++], sz, "%s: %s", k, json_object_get_string(v));
    }
  }
  op_t *o;
  JSValue p = op_begin(c, &o);
  http_request(method, url, (const char **)lines, n, body, timeout, o, fetch_done);
  for (i = 0; i < n; i++) free(lines[i]);
  free(lines);
  json_object_put(h);
  free(method), free(url), free(hjson), free(body);
  return p;
}

static void register_natives(void) {
  static const struct {
    const char *name;
    JSCFunction *fn;
    int nargs;
  } fns[] = {
      {"log", n_log, 2},          {"env", n_env, 1},
      {"sleep", n_sleep, 1},      {"exec", n_exec, 2},
      {"readFile", n_read_file, 1}, {"writeFile", n_write_file, 2},
      {"fetch", n_fetch, 5},
  };
  JSValue g = JS_GetGlobalObject(ctx);
  JSValue native = JS_NewObject(ctx);
  for (size_t i = 0; i < sizeof fns / sizeof *fns; i++)
    JS_SetPropertyStr(ctx, native, fns[i].name, JS_NewCFunction(ctx, fns[i].fn, fns[i].name, fns[i].nargs));
  JS_SetPropertyStr(ctx, g, "__native", native);
  JS_FreeValue(ctx, g);
}

/* ---------- calling into scripts ---------- */

static char *call_global_str(const char *fn, const char *arg) {
  JSValue g = JS_GetGlobalObject(ctx);
  JSValue f = JS_GetPropertyStr(ctx, g, fn);
  JSValue a = arg ? JS_NewString(ctx, arg) : JS_UNDEFINED;
  JSValue r = JS_Call(ctx, f, JS_UNDEFINED, arg ? 1 : 0, &a);
  char *out = NULL;
  if (JS_IsException(r)) dump_exception(ctx);
  else if (!JS_IsUndefined(r)) out = arg_str(ctx, r);
  JS_FreeValue(ctx, r);
  JS_FreeValue(ctx, a);
  JS_FreeValue(ctx, f);
  JS_FreeValue(ctx, g);
  return out;
}

static int call_global_bool(const char *fn, const char *arg) {
  if (!ctx) return 0;
  char *s = call_global_str(fn, arg);
  int r = s && !strcmp(s, "true");
  free(s);
  return r;
}

int script_has_hook(const char *name) { return call_global_bool("__hasHook", name); }
int script_has_tool(const char *name) { return call_global_bool("__hasTool", name); }
const char *script_tools_json(void) { return tools_json ? tools_json : "[]"; }
const char *script_config_json(void) { return config_json ? config_json : "{}"; }

typedef struct {
  void *ud;
  script_cb cb;
} pending_t;

static JSValue on_settled(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv,
                          int magic, JSValueConst *data) {
  int64_t ptr;
  JS_ToInt64(c, &ptr, data[0]);
  pending_t *p = (pending_t *)(intptr_t)ptr;
  void *ud = p->ud;
  script_cb cb = p->cb;
  free(p);
  if (magic) { /* rejected */
    char *e = error_string(c, argv[0]);
    cb(ud, e, NULL);
    free(e);
  } else if (JS_IsUndefined(argv[0])) {
    cb(ud, NULL, NULL);
  } else {
    const char *s = JS_ToCString(c, argv[0]);
    if (s && s[0] == '\x01') cb(ud, s + 1, NULL);
    else cb(ud, NULL, s);
    if (s) JS_FreeCString(c, s);
  }
  return JS_UNDEFINED;
}

void script_call(const char *name, const char *args_json, void *ud, script_cb cb) {
  JSValue g = JS_GetGlobalObject(ctx);
  JSValue f = JS_GetPropertyStr(ctx, g, "__call");
  JSValue a[2] = {JS_NewString(ctx, name), JS_NewString(ctx, args_json)};
  JSValue p = JS_Call(ctx, f, JS_UNDEFINED, 2, a);
  JS_FreeValue(ctx, a[0]);
  JS_FreeValue(ctx, a[1]);
  JS_FreeValue(ctx, f);
  JS_FreeValue(ctx, g);
  if (JS_IsException(p)) {
    JSValue ex = JS_GetException(ctx);
    char *e = error_string(ctx, ex);
    JS_FreeValue(ctx, ex);
    cb(ud, e, NULL);
    free(e);
    return;
  }
  pending_t *pd = malloc(sizeof *pd);
  pd->ud = ud;
  pd->cb = cb;
  JSValue d = JS_NewInt64(ctx, (int64_t)(intptr_t)pd);
  JSValue ok = JS_NewCFunctionData(ctx, on_settled, 1, 0, 1, &d);
  JSValue bad = JS_NewCFunctionData(ctx, on_settled, 1, 1, 1, &d);
  JSValue chained = JS_PromiseThen(ctx, p, ok, bad);
  JS_FreeValue(ctx, chained);
  JS_FreeValue(ctx, ok);
  JS_FreeValue(ctx, bad);
  JS_FreeValue(ctx, d);
  JS_FreeValue(ctx, p);
  drain_jobs();
}

/* ---------- loading ---------- */

typedef struct {
  char **v;
  int n;
} list_t;

static void list_add(list_t *l, char *s) {
  l->v = realloc(l->v, (l->n + 1) * sizeof *l->v);
  l->v[l->n++] = s;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }

static int is_script(const char *name) {
  size_t n = strlen(name);
  return (n > 3 && !strcmp(name + n - 3, ".js")) || (n > 4 && !strcmp(name + n - 4, ".mjs"));
}

/* Appends the script files in dir (sorted) to out; skip names `agent.js`
 * when skip_entry is set because it is added first by the caller. */
static void scan_dir(const char *dir, int skip_entry, list_t *out) {
  uv_fs_t req;
  if (uv_fs_scandir(NULL, &req, dir, 0, NULL) < 0) return;
  list_t names = {0};
  uv_dirent_t e;
  while (uv_fs_scandir_next(&req, &e) != UV_EOF) {
    if (e.type == UV_DIRENT_DIR || !is_script(e.name)) continue;
    if (skip_entry && !strcmp(e.name, "agent.js")) continue;
    list_add(&names, strdup(e.name));
  }
  uv_fs_req_cleanup(&req);
  qsort(names.v, names.n, sizeof *names.v, cmp_str);
  for (int i = 0; i < names.n; i++) {
    size_t sz = strlen(dir) + strlen(names.v[i]) + 2;
    char *p = malloc(sz);
    snprintf(p, sz, "%s/%s", dir, names.v[i]);
    list_add(out, p);
    free(names.v[i]);
  }
  free(names.v);
}

static char *real_path(const char *p) {
  uv_fs_t r;
  if (uv_fs_realpath(NULL, &r, p, NULL) < 0) return strdup(p);
  char *out = strdup(r.ptr);
  uv_fs_req_cleanup(&r);
  return out;
}

static int file_exists(const char *p) {
  uv_fs_t r;
  int ok = uv_fs_stat(NULL, &r, p, NULL) == 0;
  uv_fs_req_cleanup(&r);
  return ok;
}

int script_init(const char *path) {
  rt = JS_NewRuntime();
  JS_SetModuleLoaderFunc(rt, NULL, module_loader, NULL);
  JS_SetHostPromiseRejectionTracker(rt, rejection_tracker, NULL);
  ctx = JS_NewContext(rt);
  register_natives();

  JSValue r = JS_Eval(ctx, (const char *)prelude_js, sizeof prelude_js - 1, "<prelude>", 0);
  if (JS_IsException(r)) {
    dump_exception(ctx);
    return -1;
  }
  JS_FreeValue(ctx, r);

  char *root = real_path(path);
  uv_fs_t st;
  if (uv_fs_stat(NULL, &st, root, NULL) < 0) {
    fprintf(stderr, "[script] cannot open %s\n", path);
    return -1;
  }
  int is_dir = (st.statbuf.st_mode & S_IFMT) == S_IFDIR;
  uv_fs_req_cleanup(&st);

  list_t files = {0};
  if (!is_dir) {
    list_add(&files, root);
  } else {
    size_t sz = strlen(root) + 32;
    char *entry = malloc(sz), *tools = malloc(sz);
    snprintf(entry, sz, "%s/agent.js", root);
    snprintf(tools, sz, "%s/tools", root);
    if (file_exists(entry)) list_add(&files, entry);
    else free(entry);
    scan_dir(root, 1, &files);
    scan_dir(tools, 0, &files);
    free(tools);
  }

  buf_t src = {0};
  char line[4096];
  for (int i = 0; i < files.n; i++) {
    json_object *q = json_object_new_string(files.v[i]);
    int n = snprintf(line, sizeof line, "import * as m%d from %s;\n__register(%s, m%d);\n", i,
                     json_object_to_json_string(q), json_object_to_json_string(q), i);
    json_object_put(q);
    buf_append(&src, line, (size_t)n);
  }
  if (!src.data) src.data = calloc(1, 1);

  JSValue p = JS_Eval(ctx, src.data, src.len, "<bootstrap>", JS_EVAL_TYPE_MODULE);
  free(src.data);
  if (JS_IsException(p)) {
    dump_exception(ctx);
    return -1;
  }
  drain_jobs();
  while (JS_PromiseState(ctx, p) == JS_PROMISE_PENDING) { /* top-level await */
    uv_run(loop, UV_RUN_ONCE);
    drain_jobs();
  }
  int rc = 0;
  if (JS_PromiseState(ctx, p) == JS_PROMISE_REJECTED) {
    JSValue reason = JS_PromiseResult(ctx, p);
    char *e = error_string(ctx, reason);
    fprintf(stderr, "[script] failed to load: %s\n", e);
    free(e);
    JS_FreeValue(ctx, reason);
    rc = -1;
  }
  JS_FreeValue(ctx, p);
  if (rc == 0) {
    fprintf(stderr, "[script] loaded %d file%s from %s\n", files.n, files.n == 1 ? "" : "s", path);
    tools_json = call_global_str("__tools", NULL);
    config_json = call_global_str("__config", NULL);
  }
  for (int i = 0; i < files.n; i++) free(files.v[i]);
  free(files.v);
  if (is_dir) free(root);
  return rc;
}
