/*
 * agent-libuv - an LLM agent whose loop is the libuv event loop.
 *
 *   stdin line -> [onUserInput] -> [systemPrompt] -> [beforeLLM] -> LLM (HTTP)
 *     -> [afterLLM] -> tool calls: [beforeTool] -> run -> [afterTool] -> LLM ...
 *     -> final answer -> [onTurnEnd] -> next prompt
 *
 * Bracketed steps are optional JavaScript hooks (see README). The LLM and the
 * tools are asynchronous I/O; every step is a callback on the loop.
 */
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"
#include "script.h"

#define MAX_TOOL_OUTPUT (16 * 1024)
#define TOOL_TIMEOUT_MS 30000

uv_loop_t *loop;

static char api_url[512];
static const char *api_key, *model;
static json_object *messages;   /* conversation history */
static json_object *tools_json; /* tool schemas sent to the LLM */
static int busy, steps, max_steps = 20, use_builtins = 1;
static int stdin_eof, stdin_is_tty;

static uv_tty_t tty_in;
static uv_pipe_t pipe_in;
static char *linebuf;
static size_t linelen;

static void llm_request(void);
static void prompt(void);
static void process_lines(void);

/* ---------- helpers ---------- */

static json_object *args1(json_object *a) {
  json_object *arr = json_object_new_array();
  json_object_array_add(arr, a);
  return arr;
}

static int is_type(json_object *o, json_type t) { return o && json_object_is_type(o, t); }

static const char *get_str(json_object *o, const char *key) {
  json_object *v;
  return is_type(o, json_type_object) && json_object_object_get_ex(o, key, &v) &&
                 is_type(v, json_type_string)
             ? json_object_get_string(v)
             : NULL;
}

static const char *to_json(json_object *o) {
  return json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);
}

static void add_message(const char *role, const char *content, const char *tool_call_id) {
  json_object *m = json_object_new_object();
  json_object_object_add(m, "role", json_object_new_string(role));
  json_object_object_add(m, "content", json_object_new_string(content));
  if (tool_call_id) json_object_object_add(m, "tool_call_id", json_object_new_string(tool_call_id));
  json_object_array_add(messages, m);
}

/* ---------- hooks ---------- */

/* present: the hook exists and returned something other than undefined.
 * res may still be NULL (JSON null). The callee owns res. */
typedef void (*hook_cb)(void *ud, int present, json_object *res);

typedef struct {
  char *name;
  hook_cb cb;
  void *ud;
} hook_t;

static void call_hook(const char *name, json_object *args, void *ud, hook_cb cb);

static void ignore_hook(void *ud, int present, json_object *res) { json_object_put(res); }

static void report_error(const char *stage, const char *msg) {
  fprintf(stderr, "\033[31m[%s] %s\033[0m\n", stage, msg);
  if (strcmp(stage, "onError") && script_has_hook("onError")) {
    json_object *e = json_object_new_object();
    json_object_object_add(e, "stage", json_object_new_string(stage));
    json_object_object_add(e, "message", json_object_new_string(msg));
    call_hook("onError", args1(e), NULL, ignore_hook);
  }
}

static void hook_done(void *ud, const char *err, const char *json) {
  hook_t *h = ud;
  json_object *res = NULL;
  if (err) report_error(h->name, err);
  else if (json) res = json_tokener_parse(json);
  hook_cb cb = h->cb;
  void *u = h->ud;
  int present = !err && json;
  free(h->name);
  free(h);
  cb(u, present, res);
}

/* Takes ownership of args (a JSON array). A missing hook completes synchronously. */
static void call_hook(const char *name, json_object *args, void *ud, hook_cb cb) {
  if (!script_has_hook(name)) {
    json_object_put(args);
    cb(ud, 0, NULL);
    return;
  }
  hook_t *h = malloc(sizeof *h);
  h->name = strdup(name);
  h->cb = cb;
  h->ud = ud;
  script_call(name, to_json(args), h, hook_done);
  json_object_put(args);
}

/* ---------- turn lifecycle ---------- */

static void finish_turn(void) {
  busy = 0;
  process_lines(); /* run any lines buffered while we were busy */
  if (busy) return;
  if (stdin_eof) uv_stop(loop);
  else prompt();
}

static void turn_end_done(void *ud, int present, json_object *res) {
  json_object_put(res);
  finish_turn();
}

static void end_turn(const char *reply) {
  json_object *info = json_object_new_object();
  json_object_object_add(info, "steps", json_object_new_int(steps));
  json_object_object_add(info, "reply", json_object_new_string(reply ? reply : ""));
  call_hook("onTurnEnd", args1(info), NULL, turn_end_done);
}

/* ---------- tools ---------- */

typedef struct {
  char *id, *name, *result;
  json_object *args;
  int *pending; /* shared counter for the batch */
} tool_call_t;

static void tool_finish(tool_call_t *tc, const char *result) {
  if (strlen(result) > MAX_TOOL_OUTPUT) {
    char *cut = strndup(result, MAX_TOOL_OUTPUT);
    add_message("tool", cut, tc->id);
    free(cut);
  } else {
    add_message("tool", result, tc->id);
  }
  int *pending = tc->pending;
  free(tc->id);
  free(tc->name);
  free(tc->result);
  json_object_put(tc->args);
  free(tc);
  if (--*pending == 0) {
    free(pending);
    llm_request(); /* all results are in: back to the LLM */
  }
}

static void after_tool_done(void *ud, int present, json_object *res) {
  tool_call_t *tc = ud;
  const char *s = get_str(res, "result");
  if (!s && is_type(res, json_type_string)) s = json_object_get_string(res);
  tool_finish(tc, s ? s : tc->result);
  json_object_put(res);
}

/* Records the tool result and runs the afterTool hook. */
static void tool_result(tool_call_t *tc, const char *result) {
  free(tc->result);
  tc->result = strdup(result);
  json_object *info = json_object_new_object();
  json_object_object_add(info, "id", json_object_new_string(tc->id));
  json_object_object_add(info, "name", json_object_new_string(tc->name));
  json_object_object_add(info, "args", json_object_get(tc->args));
  json_object_object_add(info, "result", json_object_new_string(result));
  call_hook("afterTool", args1(info), tc, after_tool_done);
}

static void script_tool_done(void *ud, const char *err, const char *json) {
  tool_call_t *tc = ud;
  if (err) {
    report_error(tc->name, err);
    char msg[1024];
    snprintf(msg, sizeof msg, "error: %.1000s", err);
    tool_result(tc, msg);
    return;
  }
  json_object *v = json ? json_tokener_parse(json) : NULL;
  if (is_type(v, json_type_string)) tool_result(tc, json_object_get_string(v));
  else tool_result(tc, json ? json : "(no output)");
  json_object_put(v);
}

static void builtin_shell_done(void *ud, int64_t status, int timed_out, const char *out, size_t len) {
  tool_call_t *tc = ud;
  buf_t b = {0};
  buf_append(&b, out, len);
  char tail[64];
  int n = snprintf(tail, sizeof tail, "\n[exit status %lld%s]", (long long)status,
                   timed_out ? ", timed out" : "");
  buf_append(&b, tail, (size_t)n);
  tool_result(tc, b.data);
  free(b.data);
}

static void builtin_read_done(void *ud, const char *err, const char *data, size_t len) {
  tool_call_t *tc = ud;
  if (err) {
    char msg[256];
    snprintf(msg, sizeof msg, "error: %s", err);
    tool_result(tc, msg);
  } else {
    tool_result(tc, data);
  }
}

static void run_tool(tool_call_t *tc) {
  const char *arg;
  if (script_has_tool(tc->name)) {
    char call[256], *argsj = malloc(strlen(to_json(tc->args)) + 3);
    snprintf(call, sizeof call, "tool:%s", tc->name);
    sprintf(argsj, "[%s]", to_json(tc->args));
    script_call(call, argsj, tc, script_tool_done);
    free(argsj);
  } else if (use_builtins && !strcmp(tc->name, "shell") && (arg = get_str(tc->args, "command"))) {
    shell_run(arg, TOOL_TIMEOUT_MS, tc, builtin_shell_done);
  } else if (use_builtins && !strcmp(tc->name, "read_file") && (arg = get_str(tc->args, "path"))) {
    fs_read_async(arg, MAX_TOOL_OUTPUT, tc, builtin_read_done);
  } else {
    tool_result(tc, "error: unknown tool or missing arguments");
  }
}

static void before_tool_done(void *ud, int present, json_object *res) {
  tool_call_t *tc = ud;
  json_object *v;
  if (is_type(res, json_type_boolean) && !json_object_get_boolean(res)) {
    tool_result(tc, "Tool call blocked.");
  } else if (is_type(res, json_type_object)) {
    const char *short_circuit = get_str(res, "result");
    int block = json_object_object_get_ex(res, "block", &v) && json_object_get_boolean(v);
    if (short_circuit || block) {
      tool_result(tc, short_circuit ? short_circuit : "Tool call blocked.");
    } else {
      if (json_object_object_get_ex(res, "args", &v) && is_type(v, json_type_object)) {
        json_object_put(tc->args);
        tc->args = json_object_get(v);
      }
      run_tool(tc);
    }
  } else {
    run_tool(tc);
  }
  json_object_put(res);
}

static void dispatch_tool(tool_call_t *tc) {
  fprintf(stderr, "\033[2m[tool] %s %s\033[0m\n", tc->name, to_json(tc->args));
  json_object *info = json_object_new_object();
  json_object_object_add(info, "id", json_object_new_string(tc->id));
  json_object_object_add(info, "name", json_object_new_string(tc->name));
  json_object_object_add(info, "args", json_object_get(tc->args));
  call_hook("beforeTool", args1(info), tc, before_tool_done);
}

static const char *BUILTIN_TOOLS =
    "[{\"type\":\"function\",\"function\":{\"name\":\"shell\","
    "\"description\":\"Run a shell command and return combined stdout/stderr.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
    "\"required\":[\"command\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"read_file\","
    "\"description\":\"Read a text file and return its contents.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}}}]";

/* Built-ins first, minus any the script overrides, then the script's tools. */
static void build_tools(void) {
  tools_json = json_object_new_array();
  json_object *script = json_tokener_parse(script_tools_json());
  if (use_builtins) {
    json_object *builtin = json_tokener_parse(BUILTIN_TOOLS);
    for (size_t i = 0; i < json_object_array_length(builtin); i++) {
      json_object *t = json_object_array_get_idx(builtin, i), *fn;
      json_object_object_get_ex(t, "function", &fn);
      if (!script_has_tool(get_str(fn, "name"))) json_object_array_add(tools_json, json_object_get(t));
    }
    json_object_put(builtin);
  }
  for (size_t i = 0; script && i < json_object_array_length(script); i++)
    json_object_array_add(tools_json, json_object_get(json_object_array_get_idx(script, i)));
  json_object_put(script);
}

/* ---------- LLM ---------- */

typedef struct {
  json_object *root, *msg;
} llm_t;

static void handle_message(json_object *msg) {
  json_object *calls;
  if (json_object_object_get_ex(msg, "tool_calls", &calls) && is_type(calls, json_type_array) &&
      json_object_array_length(calls) > 0 && ++steps <= max_steps) {
    size_t n = json_object_array_length(calls);
    int *pending = malloc(sizeof *pending);
    *pending = (int)n;
    /* copy everything first: tools may complete synchronously */
    tool_call_t **batch = calloc(n, sizeof *batch);
    for (size_t i = 0; i < n; i++) {
      json_object *c = json_object_array_get_idx(calls, i), *fn = NULL, *args = NULL;
      json_object_object_get_ex(c, "function", &fn);
      if (fn) json_object_object_get_ex(fn, "arguments", &args);
      tool_call_t *tc = calloc(1, sizeof *tc);
      const char *id = get_str(c, "id"), *name = get_str(fn, "name");
      tc->id = strdup(id ? id : "");
      tc->name = strdup(name ? name : "");
      tc->args = args && is_type(args, json_type_string)
                     ? json_tokener_parse(json_object_get_string(args))
                     : (args ? json_object_get(args) : NULL);
      if (!is_type(tc->args, json_type_object)) {
        json_object_put(tc->args);
        tc->args = json_object_new_object();
      }
      tc->pending = pending;
      batch[i] = tc;
    }
    for (size_t i = 0; i < n; i++) dispatch_tool(batch[i]);
    free(batch);
    return;
  }

  const char *content = get_str(msg, "content");
  if (content) printf("\n%s\n\n", content);
  if (steps > max_steps) printf("(stopped: too many tool steps)\n");
  end_turn(content);
}

static void after_llm_done(void *ud, int present, json_object *res) {
  llm_t *l = ud;
  json_object *msg = is_type(res, json_type_object) ? res : l->msg;
  json_object_array_add(messages, json_object_get(msg));
  handle_message(msg);
  json_object_put(res);
  json_object_put(l->root);
  free(l);
}

static void llm_done(void *ud, const char *err, long status, const char *body, size_t len) {
  if (err) {
    report_error("llm", err);
    finish_turn();
    return;
  }
  json_object *root = json_tokener_parse(body), *choices, *msg = NULL;
  if (!root || !json_object_object_get_ex(root, "choices", &choices) ||
      json_object_array_length(choices) == 0 ||
      !json_object_object_get_ex(json_object_array_get_idx(choices, 0), "message", &msg)) {
    char text[512];
    snprintf(text, sizeof text, "HTTP %ld: %.400s", status, body);
    report_error("llm", text);
    json_object_put(root);
    finish_turn();
    return;
  }
  llm_t *l = malloc(sizeof *l);
  l->root = root;
  l->msg = msg;
  call_hook("afterLLM", args1(json_object_get(msg)), l, after_llm_done);
}

static void send_request(json_object *req) {
  const char *headers[2] = {"Content-Type: application/json", NULL};
  int n = 1;
  char auth[600];
  if (api_key && *api_key) {
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", api_key);
    headers[n++] = auth;
  }
  http_request("POST", api_url, headers, n, to_json(req), 120000, NULL, llm_done);
}

static void before_llm_done(void *ud, int present, json_object *res) {
  json_object *req = ud;
  if (is_type(res, json_type_object)) {
    json_object_put(req);
    req = res;
  } else {
    json_object_put(res);
  }
  send_request(req);
  json_object_put(req);
}

static void llm_request(void) {
  json_object *req = json_object_new_object();
  json_object_object_add(req, "model", json_object_new_string(model));
  json_object_object_add(req, "messages", json_object_get(messages));
  if (json_object_array_length(tools_json) > 0)
    json_object_object_add(req, "tools", json_object_get(tools_json));
  call_hook("beforeLLM", args1(json_object_get(req)), req, before_llm_done);
}

/* ---------- user input ---------- */

static void prompt(void) {
  if (stdin_is_tty) {
    printf("> ");
    fflush(stdout);
  }
}

static void system_prompt_done(void *ud, int present, json_object *res) {
  char *text = ud;
  if (is_type(res, json_type_string)) {
    json_object *first = json_object_array_get_idx(messages, 0);
    json_object_object_add(first, "content", json_object_get(res));
  }
  json_object_put(res);
  add_message("user", text, NULL);
  free(text);
  llm_request();
}

static void input_done(void *ud, int present, json_object *res) {
  char *text = ud;
  if (present && (!res || (is_type(res, json_type_boolean) && !json_object_get_boolean(res)))) {
    free(text); /* hook swallowed the input */
    finish_turn();
  } else if (get_str(res, "reply")) {
    printf("\n%s\n\n", get_str(res, "reply")); /* hook answered directly */
    free(text);
    finish_turn();
  } else {
    if (is_type(res, json_type_string)) {
      free(text);
      text = strdup(json_object_get_string(res));
    }
    call_hook("systemPrompt", json_object_new_array(), text, system_prompt_done);
  }
  json_object_put(res);
}

static void handle_line(char *line) {
  if (!*line) {
    prompt();
    return;
  }
  if (!strcmp(line, "/exit")) {
    uv_stop(loop);
    return;
  }
  busy = 1;
  steps = 0;
  call_hook("onUserInput", args1(json_object_new_string(line)), strdup(line), input_done);
}

/* one prompt at a time; extra buffered lines wait until the turn ends */
static void process_lines(void) {
  char *nl;
  while (!busy && linelen && (nl = memchr(linebuf, '\n', linelen))) {
    *nl = 0;
    char *line = strdup(linebuf);
    linelen -= (nl - linebuf) + 1;
    memmove(linebuf, nl + 1, linelen);
    handle_line(line);
    free(line);
  }
}

static void in_alloc_cb(uv_handle_t *h, size_t sz, uv_buf_t *b) {
  b->base = malloc(sz);
  b->len = sz;
}

static void in_read_cb(uv_stream_t *st, ssize_t n, const uv_buf_t *b) {
  if (n < 0) {
    free(b->base);
    stdin_eof = 1;
    uv_read_stop(st);
    if (!busy) uv_stop(loop);
    return;
  }
  linebuf = realloc(linebuf, linelen + n + 1);
  memcpy(linebuf + linelen, b->base, n);
  linelen += n;
  free(b->base);
  process_lines();
}

/* ---------- main ---------- */

static int exists(const char *p) {
  uv_fs_t r;
  int ok = uv_fs_stat(NULL, &r, p, NULL) == 0;
  uv_fs_req_cleanup(&r);
  return ok;
}

int main(int argc, char **argv) {
  loop = uv_default_loop();
  const char *script = argc > 1 ? argv[1] : getenv("AGENT_SCRIPT");
  if (!script && exists("agent.js")) script = "agent.js";
  if (!script && exists("agent")) script = "agent";

  http_init();
  if (script && script_init(script) != 0) return 1;

  json_object *cfg = json_tokener_parse(script_config_json()), *v;
  const char *cfg_model = get_str(cfg, "model"), *cfg_base = get_str(cfg, "baseUrl");
  if (cfg && json_object_object_get_ex(cfg, "builtins", &v)) use_builtins = json_object_get_boolean(v);
  if (cfg && json_object_object_get_ex(cfg, "maxSteps", &v)) max_steps = json_object_get_int(v);

  api_key = getenv("OPENAI_API_KEY");
  const char *base = getenv("OPENAI_BASE_URL");
  if (!base) base = cfg_base;
  if (!base) base = "https://api.openai.com/v1";
  if (!api_key && !strncmp(base, "https://api.openai.com", 22)) {
    fprintf(stderr, "Set OPENAI_API_KEY (or OPENAI_BASE_URL for a local server).\n");
    return 1;
  }
  snprintf(api_url, sizeof api_url, "%s/chat/completions", base);
  model = getenv("AGENT_MODEL");
  if (!model) model = cfg_model;
  if (!model) model = "gpt-4o-mini";

  messages = json_object_new_array();
  add_message("system",
              "You are a concise assistant running in a terminal. Use the available tools when they help.",
              NULL);
  build_tools();

  stdin_is_tty = uv_guess_handle(0) == UV_TTY;
  uv_stream_t *in = NULL;
  if (uv_guess_handle(0) == UV_FILE) {
    /* a regular file can't be polled for EOF: slurp it and replay line by line */
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, stdin)) > 0) {
      linebuf = realloc(linebuf, linelen + n + 1);
      memcpy(linebuf + linelen, chunk, n);
      linelen += n;
    }
    stdin_eof = 1;
  } else if (stdin_is_tty) {
    uv_tty_init(loop, &tty_in, 0, 1);
    in = (uv_stream_t *)&tty_in;
  } else {
    uv_pipe_init(loop, &pipe_in, 0);
    uv_pipe_open(&pipe_in, 0);
    in = (uv_stream_t *)&pipe_in;
  }
  if (in) uv_read_start(in, in_alloc_cb, in_read_cb);

  printf("agent-libuv (model: %s). /exit to quit.\n", model);
  prompt();
  process_lines();
  if (stdin_eof && !busy) return 0;
  uv_run(loop, UV_RUN_DEFAULT);
  if (stdin_is_tty) uv_tty_reset_mode();
  return 0;
}
