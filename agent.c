/*
 * agent.c - a minimal LLM agent driven by a libuv event loop.
 *
 * The agent loop is not a `while` loop; it is the libuv loop itself:
 *   stdin line (uv_read)      -> LLM request (libcurl multi, polled by uv_poll/uv_timer)
 *   LLM response (callback)   -> tool calls (uv_spawn / uv_queue_work) or final answer
 *   tool results (callbacks)  -> next LLM request
 *   final answer              -> print, wait for the next prompt
 *
 * Talks to any OpenAI-compatible /chat/completions endpoint.
 */
#include <curl/curl.h>
#include <errno.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define MAX_TOOL_OUTPUT (16 * 1024)
#define TOOL_TIMEOUT_MS 30000
#define MAX_STEPS 20

/* ---------- global state ---------- */

static uv_loop_t *loop;
static uv_timer_t curl_timer;
static CURLM *curl_multi;

static const char *api_url, *api_key, *model;
static json_object *messages; /* conversation history */
static json_object *tools;    /* tool schemas */
static int busy;              /* an agent turn is in flight */
static int steps;
static int stdin_eof;
static int stdin_is_tty;

static uv_tty_t tty_in;
static uv_pipe_t pipe_in;
static uv_stream_t *in_stream;
static char *linebuf;
static size_t linelen;

static void llm_request(void);
static void prompt(void);

/* ---------- small helpers ---------- */

typedef struct {
  char *data;
  size_t len;
} buf_t;

static void buf_append(buf_t *b, const char *p, size_t n) {
  b->data = realloc(b->data, b->len + n + 1);
  memcpy(b->data + b->len, p, n);
  b->len += n;
  b->data[b->len] = 0;
}

static void add_message(const char *role, const char *content, const char *tool_call_id) {
  json_object *m = json_object_new_object();
  json_object_object_add(m, "role", json_object_new_string(role));
  json_object_object_add(m, "content", json_object_new_string(content));
  if (tool_call_id) json_object_object_add(m, "tool_call_id", json_object_new_string(tool_call_id));
  json_object_array_add(messages, m);
}

static void process_lines(void);

static void finish_turn(void) {
  busy = 0;
  process_lines(); /* run any lines buffered while we were busy */
  if (busy) return;
  if (stdin_eof) {
    uv_stop(loop);
    return;
  }
  prompt();
}

/* ---------- tools ---------- */

typedef struct tool_call {
  char *id;
  char *name;
  json_object *args;
  buf_t out;
  int *pending; /* shared counter for the batch */
} tool_call_t;

static void tool_done(tool_call_t *tc) {
  if (tc->out.len > MAX_TOOL_OUTPUT) {
    tc->out.len = MAX_TOOL_OUTPUT;
    tc->out.data[tc->out.len] = 0;
  }
  add_message("tool", tc->out.data ? tc->out.data : "(no output)", tc->id);
  int *pending = tc->pending;
  free(tc->id);
  free(tc->name);
  json_object_put(tc->args);
  free(tc->out.data);
  free(tc);
  if (--*pending == 0) {
    free(pending);
    llm_request(); /* all tool results in: go back to the LLM */
  }
}

/* shell tool: uv_spawn with stdout+stderr piped back asynchronously */
typedef struct {
  tool_call_t *tc;
  uv_process_t proc;
  uv_pipe_t pipe;
  uv_timer_t timer;
  int open_handles;
} shell_t;

static void shell_close_cb(uv_handle_t *h) {
  shell_t *s = h->data;
  if (--s->open_handles == 0) {
    tool_done(s->tc);
    free(s);
  }
}

static void shell_maybe_close(uv_handle_t *h) {
  if (!uv_is_closing(h)) uv_close(h, shell_close_cb);
}

static void shell_alloc_cb(uv_handle_t *h, size_t sz, uv_buf_t *b) {
  b->base = malloc(sz);
  b->len = sz;
}

static void shell_read_cb(uv_stream_t *st, ssize_t n, const uv_buf_t *b) {
  shell_t *s = st->data;
  if (n > 0) buf_append(&s->tc->out, b->base, (size_t)n);
  free(b->base);
  if (n < 0) shell_maybe_close((uv_handle_t *)st);
}

static void shell_exit_cb(uv_process_t *p, int64_t status, int sig) {
  shell_t *s = p->data;
  char msg[64];
  snprintf(msg, sizeof msg, "\n[exit status %lld]", (long long)status);
  buf_append(&s->tc->out, msg, strlen(msg));
  shell_maybe_close((uv_handle_t *)&s->timer);
  shell_maybe_close((uv_handle_t *)p);
}

static void shell_timeout_cb(uv_timer_t *t) {
  shell_t *s = t->data;
  uv_process_kill(&s->proc, SIGKILL);
  buf_append(&s->tc->out, "\n[timed out]", 12);
}

static void tool_shell(tool_call_t *tc) {
  json_object *cmd;
  if (!json_object_object_get_ex(tc->args, "command", &cmd)) {
    buf_append(&tc->out, "error: missing 'command'", 24);
    tool_done(tc);
    return;
  }
  shell_t *s = calloc(1, sizeof *s);
  s->tc = tc;
  s->open_handles = 3; /* process, pipe, timer */

  const char *user_cmd = json_object_get_string(cmd);
  size_t full_len = strlen(user_cmd) + 16;
  char *full = malloc(full_len);
  snprintf(full, full_len, "(%s) 2>&1", user_cmd);
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
    buf_append(&tc->out, "spawn failed: ", 14);
    buf_append(&tc->out, e, strlen(e));
    /* proc was never started; close the other handles and finish */
    s->open_handles = 2;
    uv_close((uv_handle_t *)&s->pipe, shell_close_cb);
    uv_close((uv_handle_t *)&s->timer, shell_close_cb);
    return;
  }
  uv_read_start((uv_stream_t *)&s->pipe, shell_alloc_cb, shell_read_cb);
  uv_timer_start(&s->timer, shell_timeout_cb, TOOL_TIMEOUT_MS, 0);
}

/* read_file tool: blocking fs work runs on libuv's threadpool */
typedef struct {
  uv_work_t req;
  tool_call_t *tc;
} work_t;

static void read_file_work(uv_work_t *w) {
  tool_call_t *tc = ((work_t *)w->data)->tc;
  json_object *p;
  if (!json_object_object_get_ex(tc->args, "path", &p)) {
    buf_append(&tc->out, "error: missing 'path'", 21);
    return;
  }
  FILE *f = fopen(json_object_get_string(p), "r");
  if (!f) {
    const char *e = strerror(errno);
    buf_append(&tc->out, "error: ", 7);
    buf_append(&tc->out, e, strlen(e));
    return;
  }
  char chunk[4096];
  size_t n;
  while (tc->out.len < MAX_TOOL_OUTPUT && (n = fread(chunk, 1, sizeof chunk, f)) > 0)
    buf_append(&tc->out, chunk, n);
  fclose(f);
}

static void read_file_after(uv_work_t *w, int status) {
  work_t *wk = w->data;
  tool_done(wk->tc);
  free(wk);
}

static void tool_read_file(tool_call_t *tc) {
  work_t *wk = calloc(1, sizeof *wk);
  wk->tc = tc;
  wk->req.data = wk;
  uv_queue_work(loop, &wk->req, read_file_work, read_file_after);
}

static void dispatch_tool(tool_call_t *tc) {
  fprintf(stderr, "\033[2m[tool] %s %s\033[0m\n", tc->name, json_object_to_json_string(tc->args));
  if (!strcmp(tc->name, "shell")) tool_shell(tc);
  else if (!strcmp(tc->name, "read_file")) tool_read_file(tc);
  else {
    buf_append(&tc->out, "error: unknown tool", 19);
    tool_done(tc);
  }
}

static void build_tools(void) {
  const char *defs =
      "[{\"type\":\"function\",\"function\":{\"name\":\"shell\","
      "\"description\":\"Run a shell command and return combined stdout/stderr.\","
      "\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
      "\"required\":[\"command\"]}}},"
      "{\"type\":\"function\",\"function\":{\"name\":\"read_file\","
      "\"description\":\"Read a text file and return its contents.\","
      "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
      "\"required\":[\"path\"]}}}]";
  tools = json_tokener_parse(defs);
}

/* ---------- LLM response handling ---------- */

static void handle_response(const char *body, long http_code) {
  json_object *root = json_tokener_parse(body ? body : "");
  json_object *choices, *msg;
  if (!root || !json_object_object_get_ex(root, "choices", &choices) ||
      json_object_array_length(choices) == 0) {
    fprintf(stderr, "LLM error (HTTP %ld): %s\n", http_code, body ? body : "");
    if (root) json_object_put(root);
    finish_turn();
    return;
  }
  json_object *choice = json_object_array_get_idx(choices, 0);
  json_object_object_get_ex(choice, "message", &msg);
  /* keep the assistant message (including tool_calls) in the history */
  json_object_array_add(messages, json_object_get(msg));

  json_object *calls;
  if (json_object_object_get_ex(msg, "tool_calls", &calls) && json_object_array_length(calls) > 0 &&
      ++steps <= MAX_STEPS) {
    size_t n = json_object_array_length(calls);
    int *pending = malloc(sizeof *pending);
    *pending = (int)n;
    /* copy ids/names/args up front: tools may complete synchronously */
    tool_call_t **batch = calloc(n, sizeof *batch);
    for (size_t i = 0; i < n; i++) {
      json_object *c = json_object_array_get_idx(calls, i), *id, *fn, *name, *args;
      json_object_object_get_ex(c, "id", &id);
      json_object_object_get_ex(c, "function", &fn);
      json_object_object_get_ex(fn, "name", &name);
      json_object_object_get_ex(fn, "arguments", &args);
      tool_call_t *tc = calloc(1, sizeof *tc);
      tc->id = strdup(json_object_get_string(id));
      tc->name = strdup(json_object_get_string(name));
      tc->args = json_tokener_parse(json_object_get_string(args));
      if (!tc->args) tc->args = json_object_new_object();
      tc->pending = pending;
      batch[i] = tc;
    }
    json_object_put(root);
    for (size_t i = 0; i < n; i++) dispatch_tool(batch[i]);
    free(batch);
    return;
  }

  json_object *content;
  if (json_object_object_get_ex(msg, "content", &content) && content)
    printf("\n%s\n\n", json_object_get_string(content));
  if (steps > MAX_STEPS) printf("(stopped: too many tool steps)\n");
  json_object_put(root);
  finish_turn();
}

/* ---------- libcurl <-> libuv glue (curl "multi socket" API) ---------- */

typedef struct {
  CURL *easy;
  struct curl_slist *headers;
  char *payload;
  buf_t resp;
} request_t;

typedef struct {
  uv_poll_t poll;
  curl_socket_t sock;
} curl_sock_t;

static void check_multi_info(void) {
  CURLMsg *m;
  int left;
  while ((m = curl_multi_info_read(curl_multi, &left))) {
    if (m->msg != CURLMSG_DONE) continue;
    request_t *rq;
    curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, (char **)&rq);
    long code = 0;
    curl_easy_getinfo(m->easy_handle, CURLINFO_RESPONSE_CODE, &code);
    CURLcode res = m->data.result;
    curl_multi_remove_handle(curl_multi, m->easy_handle);
    curl_easy_cleanup(rq->easy);
    curl_slist_free_all(rq->headers);
    free(rq->payload);
    char *body = rq->resp.data;
    free(rq);
    if (res != CURLE_OK) {
      fprintf(stderr, "request failed: %s\n", curl_easy_strerror(res));
      free(body);
      finish_turn();
    } else {
      handle_response(body, code);
      free(body);
    }
  }
}

static void poll_cb(uv_poll_t *p, int status, int events) {
  curl_sock_t *cs = p->data;
  int flags = 0, running;
  if (events & UV_READABLE) flags |= CURL_CSELECT_IN;
  if (events & UV_WRITABLE) flags |= CURL_CSELECT_OUT;
  curl_multi_socket_action(curl_multi, cs->sock, flags, &running);
  check_multi_info();
}

static void sock_close_cb(uv_handle_t *h) { free(h->data); }

static int socket_cb(CURL *e, curl_socket_t s, int what, void *up, void *sp) {
  curl_sock_t *cs = sp;
  if (what == CURL_POLL_REMOVE) {
    if (cs) {
      uv_poll_stop(&cs->poll);
      uv_close((uv_handle_t *)&cs->poll, sock_close_cb);
      curl_multi_assign(curl_multi, s, NULL);
    }
    return 0;
  }
  if (!cs) {
    cs = calloc(1, sizeof *cs);
    cs->sock = s;
    uv_poll_init_socket(loop, &cs->poll, s);
    cs->poll.data = cs;
    curl_multi_assign(curl_multi, s, cs);
  }
  int ev = 0;
  if (what != CURL_POLL_IN) ev |= UV_WRITABLE;
  if (what != CURL_POLL_OUT) ev |= UV_READABLE;
  uv_poll_start(&cs->poll, ev, poll_cb);
  return 0;
}

static void timer_cb(uv_timer_t *t) {
  int running;
  curl_multi_socket_action(curl_multi, CURL_SOCKET_TIMEOUT, 0, &running);
  check_multi_info();
}

static int timer_update_cb(CURLM *m, long ms, void *up) {
  if (ms < 0) uv_timer_stop(&curl_timer);
  else uv_timer_start(&curl_timer, timer_cb, ms == 0 ? 1 : ms, 0);
  return 0;
}

static size_t write_cb(char *p, size_t sz, size_t n, void *ud) {
  buf_append(&((request_t *)ud)->resp, p, sz * n);
  return sz * n;
}

static void llm_request(void) {
  json_object *body = json_object_new_object();
  json_object_object_add(body, "model", json_object_new_string(model));
  json_object_object_add(body, "messages", json_object_get(messages));
  json_object_object_add(body, "tools", json_object_get(tools));

  request_t *rq = calloc(1, sizeof *rq);
  rq->payload = strdup(json_object_to_json_string(body));
  json_object_put(body);

  char auth[512];
  snprintf(auth, sizeof auth, "Authorization: Bearer %s", api_key);
  rq->headers = curl_slist_append(NULL, "Content-Type: application/json");
  rq->headers = curl_slist_append(rq->headers, auth);

  rq->easy = curl_easy_init();
  curl_easy_setopt(rq->easy, CURLOPT_URL, api_url);
  curl_easy_setopt(rq->easy, CURLOPT_HTTPHEADER, rq->headers);
  curl_easy_setopt(rq->easy, CURLOPT_POSTFIELDS, rq->payload);
  curl_easy_setopt(rq->easy, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(rq->easy, CURLOPT_WRITEDATA, rq);
  curl_easy_setopt(rq->easy, CURLOPT_PRIVATE, rq);
  curl_easy_setopt(rq->easy, CURLOPT_TIMEOUT, 120L);
  curl_multi_add_handle(curl_multi, rq->easy);
}

/* ---------- user input ---------- */

static void prompt(void) {
  if (stdin_is_tty) {
    printf("> ");
    fflush(stdout);
  }
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
  add_message("user", line, NULL);
  llm_request();
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

int main(void) {
  api_key = getenv("OPENAI_API_KEY");
  if (!api_key) {
    fprintf(stderr, "Set OPENAI_API_KEY (and optionally OPENAI_BASE_URL, AGENT_MODEL).\n");
    return 1;
  }
  const char *base = getenv("OPENAI_BASE_URL");
  static char url[512];
  snprintf(url, sizeof url, "%s/chat/completions", base ? base : "https://api.openai.com/v1");
  api_url = url;
  model = getenv("AGENT_MODEL") ? getenv("AGENT_MODEL") : "gpt-4o-mini";

  messages = json_object_new_array();
  add_message("system", "You are a concise assistant running in a terminal. Use the shell and read_file tools when they help.", NULL);
  build_tools();

  loop = uv_default_loop();
  stdin_is_tty = uv_guess_handle(0) == UV_TTY;
  curl_global_init(CURL_GLOBAL_ALL);
  curl_multi = curl_multi_init();
  curl_multi_setopt(curl_multi, CURLMOPT_SOCKETFUNCTION, socket_cb);
  curl_multi_setopt(curl_multi, CURLMOPT_TIMERFUNCTION, timer_update_cb);
  uv_timer_init(loop, &curl_timer);

  if (stdin_is_tty) {
    uv_tty_init(loop, &tty_in, 0, 1);
    in_stream = (uv_stream_t *)&tty_in;
  } else {
    uv_pipe_init(loop, &pipe_in, 0);
    uv_pipe_open(&pipe_in, 0);
    in_stream = (uv_stream_t *)&pipe_in;
  }
  uv_read_start(in_stream, in_alloc_cb, in_read_cb);

  printf("agent-libuv (model: %s). /exit to quit.\n", model);
  prompt();
  uv_run(loop, UV_RUN_DEFAULT);
  if (stdin_is_tty) uv_tty_reset_mode();
  return 0;
}
