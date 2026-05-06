/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 CEMAXECUTER LLC
 *
 * Outbound DEALER C2 socket. Single background thread, single socket.
 * Heartbeats every HEARTBEAT_S seconds; in between, a poll waits for
 * inbound command frames. Commands dispatch into c2.c shared handlers.
 */

#define _GNU_SOURCE
#include "c2_dealer.h"
#include "c2.h"
#include "options.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_ZMQ

#include <zmq.h>

#define HEARTBEAT_S       30
#define POLL_TIMEOUT_MS   1000

static struct {
    void       *ctx;
    void       *sock;
    pthread_t   thr;
    atomic_int  running;
    char        endpoint[256];
} D;

/* Tiny JSON field extractor for the envelope. We control both ends so
 * a strict JSON parser would be overkill. Returns a malloc'd copy of
 * the value (caller frees), or NULL if not found. Handles strings
 * (quoted) and integers (unquoted). */
static char *json_extract(const char *s, const char *field)
{
    if (!s || !field) return NULL;
    char needle[64];
    int nl = snprintf(needle, sizeof(needle), "\"%s\"", field);
    if (nl <= 0 || nl >= (int)sizeof(needle)) return NULL;
    const char *p = strstr(s, needle);
    if (!p) return NULL;
    p += nl;
    while (*p == ' ' || *p == ':' || *p == '\t') ++p;
    if (*p == '"') {
        ++p;
        const char *q = p;
        while (*q && *q != '"') {
            if (*q == '\\' && q[1]) q += 2; else ++q;
        }
        if (*q != '"') return NULL;
        size_t n = (size_t)(q - p);
        char *out = malloc(n + 1);
        if (!out) return NULL;
        /* Unescape \" \\ \n \t -- enough for our uses. */
        size_t j = 0;
        for (size_t i = 0; i < n; ++i) {
            if (p[i] == '\\' && i + 1 < n) {
                char c = p[i + 1];
                if      (c == 'n')  out[j++] = '\n';
                else if (c == 't')  out[j++] = '\t';
                else if (c == 'r')  out[j++] = '\r';
                else                 out[j++] = c;
                ++i;
            } else {
                out[j++] = p[i];
            }
        }
        out[j] = 0;
        return out;
    }
    /* Unquoted value: integer / true / false / null. Read up to the
     * next non-numeric/identifier char. */
    const char *q = p;
    while (*q && *q != ',' && *q != '}' && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t') ++q;
    size_t n = (size_t)(q - p);
    if (!n) return NULL;
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, p, n);
    out[n] = 0;
    return out;
}

static void send_heartbeat(void)
{
    if (!D.sock) return;
    char buf[256];
    const char *station = opt_station_id ? opt_station_id : "sniffer";
    int n = snprintf(buf, sizeof(buf),
        "{\"event\":\"HEARTBEAT\",\"station\":\"%s\",\"ts\":%ld}",
        station, (long)time(NULL));
    if (n > 0 && n < (int)sizeof(buf)) {
        zmq_send(D.sock, buf, (size_t)n, ZMQ_DONTWAIT);
    }
}

static void handle_command(const char *frame, size_t frame_len)
{
    /* Cap inbound frame at 64 KiB. Any legitimate C2 envelope is well
     * under this; the cap also prevents a malicious peer from forcing
     * a giant alloc, and keeps the malloc(frame_len + 1) below from
     * underflowing if frame_len ever reached SIZE_MAX. */
    if (frame_len == 0 || frame_len > 65536) return;
    /* Copy frame into a NUL-terminated buffer for json_extract. */
    char *zframe = malloc(frame_len + 1);
    if (!zframe) return;
    memcpy(zframe, frame, frame_len);
    zframe[frame_len] = 0;

    char *cmd  = json_extract(zframe, "cmd");
    char *body = json_extract(zframe, "body");
    char *idstr= json_extract(zframe, "id");

    c2_response_t resp = {0};
    c2_dispatch(cmd, body, &resp);

    /* Reply envelope: forward the id verbatim if present so the caller
     * can pair request/response. body is the handler's JSON output. */
    char reply[512];
    int rn;
    if (idstr) {
        rn = snprintf(reply, sizeof(reply),
            "{\"id\":%s,\"status\":%d,\"body\":%s}",
            idstr, resp.status, resp.body[0] ? resp.body : "null");
    } else {
        rn = snprintf(reply, sizeof(reply),
            "{\"status\":%d,\"body\":%s}",
            resp.status, resp.body[0] ? resp.body : "null");
    }
    if (rn > 0 && rn < (int)sizeof(reply)) {
        zmq_send(D.sock, reply, (size_t)rn, ZMQ_DONTWAIT);
    }
    free(cmd); free(body); free(idstr); free(zframe);
}

static void *dealer_thread(void *arg)
{
    (void)arg;
    time_t last_hb = 0;
    while (atomic_load(&D.running)) {
        zmq_pollitem_t items[1] = {{ D.sock, 0, ZMQ_POLLIN, 0 }};
        int rc = zmq_poll(items, 1, POLL_TIMEOUT_MS);
        if (rc < 0) {
            if (zmq_errno() == ETERM) break;
            continue;
        }
        if (rc > 0 && (items[0].revents & ZMQ_POLLIN)) {
            zmq_msg_t msg;
            zmq_msg_init(&msg);
            int got = zmq_msg_recv(&msg, D.sock, ZMQ_DONTWAIT);
            if (got > 0) {
                handle_command(zmq_msg_data(&msg), zmq_msg_size(&msg));
            }
            zmq_msg_close(&msg);
        }
        time_t now = time(NULL);
        if (now - last_hb >= HEARTBEAT_S) {
            send_heartbeat();
            last_hb = now;
        }
    }
    return NULL;
}

bool c2_dealer_init(const char *endpoint)
{
    if (!endpoint || !*endpoint) return false;
    snprintf(D.endpoint, sizeof(D.endpoint), "%s", endpoint);

    D.ctx = zmq_ctx_new();
    if (!D.ctx) { fprintf(stderr, "c2-dealer: ctx_new failed\n"); return false; }
    D.sock = zmq_socket(D.ctx, ZMQ_DEALER);
    if (!D.sock) {
        fprintf(stderr, "c2-dealer: socket failed\n");
        zmq_ctx_term(D.ctx); D.ctx = NULL; return false;
    }
    /* ROUTER-side wants to see who we are; use station-id as identity. */
    const char *station = opt_station_id ? opt_station_id : "sniffer";
    zmq_setsockopt(D.sock, ZMQ_IDENTITY, station, strlen(station));
    int linger = 0;
    zmq_setsockopt(D.sock, ZMQ_LINGER, &linger, sizeof(linger));
    int hwm = 100;
    zmq_setsockopt(D.sock, ZMQ_SNDHWM, &hwm, sizeof(hwm));
    zmq_setsockopt(D.sock, ZMQ_RCVHWM, &hwm, sizeof(hwm));
    /* DEALER reconnects automatically on disconnect; nothing to do here. */

    if (zmq_connect(D.sock, D.endpoint) != 0) {
        fprintf(stderr, "c2-dealer: connect %s failed: %s\n",
                D.endpoint, zmq_strerror(zmq_errno()));
        zmq_close(D.sock); D.sock = NULL;
        zmq_ctx_term(D.ctx); D.ctx = NULL;
        return false;
    }

    atomic_store(&D.running, 1);
    if (pthread_create(&D.thr, NULL, dealer_thread, NULL) != 0) {
        atomic_store(&D.running, 0);
        zmq_close(D.sock); D.sock = NULL;
        zmq_ctx_term(D.ctx); D.ctx = NULL;
        return false;
    }
    fprintf(stderr, "c2-dealer: connected to %s as identity '%s'\n", D.endpoint, station);
    return true;
}

void c2_dealer_shutdown(void)
{
    if (!atomic_load(&D.running)) return;
    atomic_store(&D.running, 0);
    /* Force the poll to return. */
    if (D.ctx) zmq_ctx_shutdown(D.ctx);
    pthread_join(D.thr, NULL);
    if (D.sock) { zmq_close(D.sock); D.sock = NULL; }
    if (D.ctx)  { zmq_ctx_term(D.ctx); D.ctx = NULL; }
}

#else  /* !HAVE_ZMQ */

bool c2_dealer_init(const char *endpoint)
{
    if (endpoint && *endpoint)
        fprintf(stderr, "c2-dealer: built without libzmq; --c2-dealer ignored\n");
    return false;
}
void c2_dealer_shutdown(void) {}

#endif
