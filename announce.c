/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 CEMAXECUTER LLC
 *
 * Fusion auto-announce: background thread that periodically POSTs
 * this sniffer's registry entry to a meshtastic-fusion /api/sensors
 * endpoint. Hand-rolled HTTP/1.1 client matching the project style.
 */

#define _GNU_SOURCE
#include "announce.h"
#include "options.h"
#include "gpsd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define ANNOUNCE_INTERVAL_S 30
#define ANNOUNCE_TIMEOUT_S  5

static struct {
    char host[256];
    int  port;
    char path[256];
    pthread_t thr;
    atomic_int running;
} A;

/* Parse "http://host[:port]/path". Stores into A.host/port/path. */
static bool parse_url(const char *url)
{
    const char *p = url;
    if (strncmp(p, "http://", 7) != 0) return false;
    p += 7;
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(hostend - p));
    size_t hlen = colon ? (size_t)(colon - p) : (size_t)(hostend - p);
    if (hlen == 0 || hlen >= sizeof(A.host)) return false;
    memcpy(A.host, p, hlen);
    A.host[hlen] = '\0';
    A.port = colon ? atoi(colon + 1) : 80;
    if (A.port <= 0 || A.port > 65535) return false;
    if (slash) {
        size_t plen = strlen(slash);
        if (plen >= sizeof(A.path)) return false;
        memcpy(A.path, slash, plen + 1);
    } else {
        strcpy(A.path, "/api/sensors");
    }
    return true;
}

static int connect_with_timeout(const char *host, int port, int timeout_s)
{
    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portbuf, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    struct timeval tv = { .tv_sec = timeout_s };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Build the JSON sensor entry. Web port + station id come from opts;
 * lat/lon/alt come from gpsd if a recent fix exists. */
static int build_body(char *out, size_t cap)
{
    const char *name = opt_station_id ? opt_station_id : "sniffer";
    char zmq_buf[128] = "";
    if (opt_zmq_endpoint) snprintf(zmq_buf, sizeof(zmq_buf), "%s", opt_zmq_endpoint);
    char api_buf[128] = "";
    if (opt_web_port > 0) snprintf(api_buf, sizeof(api_buf), "http://localhost:%d", opt_web_port);

    double lat=0, lon=0, alt=0, age=0;
    bool have_fix = gpsd_get_fix(&lat, &lon, &alt, &age) && age < 60.0;

    /* Chained snprintf: each step must verify it neither failed nor
     * filled the buffer, otherwise (cap - n) underflows on the next
     * call and writes past `out`. Caller treats a 0 return as failure. */
#define APPEND(fmt, ...) do { \
        if (n < 0 || (size_t)n >= cap) return 0; \
        int _w = snprintf(out + n, cap - (size_t)n, (fmt), ##__VA_ARGS__); \
        if (_w < 0) return 0; \
        n += _w; \
    } while (0)

    int n = snprintf(out, cap, "{\"name\":\"%s\"", name);
    if (n < 0 || (size_t)n >= cap) return 0;
    if (zmq_buf[0]) APPEND(",\"zmq\":\"%s\"", zmq_buf);
    if (api_buf[0]) APPEND(",\"api\":\"%s\"", api_buf);
    if (have_fix) APPEND(",\"lat\":%.7f,\"lon\":%.7f,\"alt_m\":%.1f", lat, lon, alt);
    APPEND("}");
    return n;
#undef APPEND
}

static void post_once(void)
{
    int fd = connect_with_timeout(A.host, A.port, ANNOUNCE_TIMEOUT_S);
    if (fd < 0) {
        fprintf(stderr, "announce: connect %s:%d failed: %s\n", A.host, A.port, strerror(errno));
        return;
    }
    char body[1024];
    int blen = build_body(body, sizeof(body));
    if (blen <= 0 || blen >= (int)sizeof(body)) { close(fd); return; }

    char req[2048];
    int rlen = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        A.path, A.host, A.port, blen, body);
    if (rlen <= 0 || rlen >= (int)sizeof(req)) { close(fd); return; }

    ssize_t off = 0;
    while (off < rlen) {
        ssize_t n = send(fd, req + off, (size_t)(rlen - off), MSG_NOSIGNAL);
        if (n <= 0) { close(fd); return; }
        off += n;
    }
    /* Read just enough to parse status line, ignore body. */
    char resp[256] = {0};
    ssize_t got = recv(fd, resp, sizeof(resp) - 1, 0);
    close(fd);
    if (got <= 0) {
        fprintf(stderr, "announce: no response from %s:%d\n", A.host, A.port);
        return;
    }
    int status = 0;
    if (sscanf(resp, "HTTP/1.%*d %d", &status) == 1 && status >= 200 && status < 300) {
        /* success path stays quiet to keep stderr clean. */
    } else {
        fprintf(stderr, "announce: %s:%d responded with: %.40s\n", A.host, A.port, resp);
    }
}

static void *announce_thread(void *arg)
{
    (void)arg;
    /* First post on a short delay so the sensor is up first. */
    int initial = 3;
    for (int i = 0; i < initial && atomic_load(&A.running); ++i) sleep(1);
    while (atomic_load(&A.running)) {
        post_once();
        for (int i = 0; i < ANNOUNCE_INTERVAL_S && atomic_load(&A.running); ++i) sleep(1);
    }
    return NULL;
}

bool announce_init(const char *url)
{
    if (!url || !*url) return false;
    if (!parse_url(url)) {
        fprintf(stderr, "announce: malformed URL '%s' (expected http://host[:port]/path)\n", url);
        return false;
    }
    atomic_store(&A.running, 1);
    if (pthread_create(&A.thr, NULL, announce_thread, NULL) != 0) {
        atomic_store(&A.running, 0);
        return false;
    }
    fprintf(stderr, "announce: posting sensor entry to http://%s:%d%s every %ds\n",
            A.host, A.port, A.path, ANNOUNCE_INTERVAL_S);
    return true;
}

void announce_shutdown(void)
{
    if (!atomic_load(&A.running)) return;
    atomic_store(&A.running, 0);
    pthread_join(A.thr, NULL);
}
