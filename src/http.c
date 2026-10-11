/* libcurl "multi socket" integration with libuv. */
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

static uv_timer_t curl_timer;
static CURLM *curl_multi;

typedef struct {
  CURL *easy;
  struct curl_slist *headers;
  char *payload;
  buf_t resp;
  void *ud;
  http_cb cb;
  char *err;
  long code;
} request_t;

typedef struct {
  uv_poll_t poll;
  curl_socket_t sock;
} curl_sock_t;

static void deliver(void *p) {
  request_t *rq = p;
  rq->cb(rq->ud, rq->err, rq->code, rq->resp.data ? rq->resp.data : "", rq->resp.len);
  free(rq->err);
  free(rq->resp.data);
  free(rq);
}

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
    rq->code = code;
    if (res != CURLE_OK) rq->err = strdup(curl_easy_strerror(res));
    /* Deliver on the next tick: callbacks often start a new request, and libuv
     * must finish closing this connection's poll handle before the fd is reused. */
    timer_after(0, rq, deliver);
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

void http_init(void) {
  curl_global_init(CURL_GLOBAL_ALL);
  curl_multi = curl_multi_init();
  curl_multi_setopt(curl_multi, CURLMOPT_SOCKETFUNCTION, socket_cb);
  curl_multi_setopt(curl_multi, CURLMOPT_TIMERFUNCTION, timer_update_cb);
  uv_timer_init(loop, &curl_timer);
}

void http_request(const char *method, const char *url, const char **headers, int nheaders,
                  const char *body, long timeout_ms, void *ud, http_cb cb) {
  request_t *rq = calloc(1, sizeof *rq);
  rq->ud = ud;
  rq->cb = cb;
  for (int i = 0; i < nheaders; i++) rq->headers = curl_slist_append(rq->headers, headers[i]);

  rq->easy = curl_easy_init();
  curl_easy_setopt(rq->easy, CURLOPT_URL, url);
  curl_easy_setopt(rq->easy, CURLOPT_CUSTOMREQUEST, method);
  curl_easy_setopt(rq->easy, CURLOPT_HTTPHEADER, rq->headers);
  if (body && *body) {
    rq->payload = strdup(body);
    curl_easy_setopt(rq->easy, CURLOPT_POSTFIELDS, rq->payload);
  }
  curl_easy_setopt(rq->easy, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(rq->easy, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(rq->easy, CURLOPT_WRITEDATA, rq);
  curl_easy_setopt(rq->easy, CURLOPT_PRIVATE, rq);
  curl_easy_setopt(rq->easy, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 120000L);
  curl_multi_add_handle(curl_multi, rq->easy);
}
