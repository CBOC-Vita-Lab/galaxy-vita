// Plain HTTP/1.1 for the desktop test builds (the local fake of the online service). One detached thread per request: connect, send,
// read the answer (Content-Length, chunked or until close), with time limits. Not built for the Vita.
#include "np_http.h"

#if !defined(NP_TARGET_VITA)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace NpNet {
namespace {

uint64_t MonoMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec) / 1000000;
}

// One request. state: 0 running, 1 finished (the owner takes the answer and deletes it), 2 abandoned (the thread deletes it).
struct Req {
    volatile int state;
    std::string method, host, path, body, extra;
    uint16_t port;
    uint32_t timeoutMs;
    uint32_t ioTimeoutMs;   // netport31: the longest wait for any byte once connected (the console client's rule, np_https.h io_timeout_ms)
    HttpResponse res;
    Req() : state(0), port(80), timeoutMs(10000), ioTimeoutMs(10000) {}
};

// netport31: the no-data limit of a request that names none: 10 s, as np_https on the console, so a host test sees what the console sees
// (a held poll that passes no limit of its own fails after 10 s without data). Tests shrink it (HttpTestSetDefaultIoMs).
uint32_t g_defaultIoMs = 0;
uint32_t DefaultIoMs() { return g_defaultIoMs ? g_defaultIoMs : 10000; }
int g_live = 0;   // sockets open now (tests: every failing or abandoned request closes its socket)

// Waits for the socket, in slices: false at the deadline, or at once once the owner gave the request up (an abandoned request closes its
// socket now, not at the end of its time limit: a cancelled held poll does not keep a socket for its whole hold).
bool WaitFd(const Req* r, int fd, short events, uint64_t deadline) {
    for (;;) {
        const uint64_t now = MonoMs();
        if (now >= deadline) return false;
        if (__atomic_load_n(&r->state, __ATOMIC_ACQUIRE) == 2) return false;
        pollfd p;
        p.fd = fd;
        p.events = events;
        p.revents = 0;
        const uint64_t left = deadline - now;
        const int rc = poll(&p, 1, int(left > 50 ? 50 : left));
        if (rc > 0) return true;
        if (rc < 0 && errno != EINTR) return false;
    }
}

int OpenSocket() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) __atomic_add_fetch(&g_live, 1, __ATOMIC_RELAXED);
    return fd;
}
void CloseSocket(int fd) {
    ::close(fd);
    __atomic_sub_fetch(&g_live, 1, __ATOMIC_RELAXED);
}

void Fail(Req* r, const char* text) {
    r->res.error = 1;
    r->res.status = 0;
    r->res.text = text;
}

void Run(Req* r) {
    const uint64_t deadline = MonoMs() + r->timeoutMs;
    addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = 0;
    char portText[8];
    snprintf(portText, sizeof portText, "%u", unsigned(r->port));
    if (getaddrinfo(r->host.c_str(), portText, &hints, &res) != 0 || !res) { Fail(r, "The server name could not be found."); return; }
    const int fd = OpenSocket();
    if (fd < 0) { freeaddrinfo(res); Fail(r, "No socket."); return; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0 && errno != EINPROGRESS) { CloseSocket(fd); Fail(r, "The server refused the connection."); return; }
    if (rc != 0) {
        int err = 0;
        socklen_t l = sizeof err;
        if (!WaitFd(r, fd, POLLOUT, deadline) || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) != 0 || err) { CloseSocket(fd); Fail(r, "Could not connect to the server."); return; }
    }
    std::string req = r->method + " " + r->path + " HTTP/1.1\r\nHost: " + r->host + "\r\nUser-Agent: GalaxyVita\r\nConnection: close\r\nAccept-Encoding: identity\r\n";   // (the same User-Agent as np_https: a request without one can be refused before the Worker runs)
    req += r->extra;
    if (!r->body.empty() || r->method == "POST" || r->method == "PUT") {
        char cl[64];
        snprintf(cl, sizeof cl, "Content-Length: %u\r\n", unsigned(r->body.size()));
        req += cl;
    }
    req += "\r\n";
    req += r->body;
    size_t sent = 0;
    while (sent < req.size()) {
        if (!WaitFd(r, fd, POLLOUT, deadline)) { CloseSocket(fd); Fail(r, "The server did not accept the request in time."); return; }
        const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
        if (n < 0 && errno != EAGAIN && errno != EINTR) { CloseSocket(fd); Fail(r, "The connection to the server broke."); return; }
        if (n > 0) sent += size_t(n);
    }
    std::string in;
    char buf[4096];
    uint64_t lastByte = MonoMs();   // (the request is sent: the no-data limit runs from here)
    bool closed = false;
    size_t headEnd = std::string::npos;
    long contentLength = -1;
    bool chunked = false;
    for (;;) {
        if (headEnd == std::string::npos) {
            headEnd = in.find("\r\n\r\n");
            if (headEnd != std::string::npos) {
                std::string head = in.substr(0, headEnd);
                for (size_t i = 0; i < head.size(); ++i) head[i] = char(tolower(static_cast<unsigned char>(head[i])));
                size_t p = head.find("content-length:");
                if (p != std::string::npos) contentLength = atol(head.c_str() + p + 15);
                chunked = head.find("transfer-encoding: chunked") != std::string::npos;
                headEnd += 4;
            }
        }
        if (headEnd != std::string::npos) {
            if (!chunked && contentLength >= 0 && in.size() - headEnd >= size_t(contentLength)) break;
            if (chunked && in.size() >= 5 && in.compare(in.size() - 5, 5, "0\r\n\r\n") == 0) break;
        }
        if (closed) break;
        if (in.size() > 256 * 1024) { CloseSocket(fd); Fail(r, "The answer is too large."); return; }
        // the answer may be quiet for at most ioTimeoutMs at a time (a held poll asks for its hold + a margin), and the whole request timeoutMs
        const uint64_t quietEnd = lastByte + r->ioTimeoutMs;
        if (!WaitFd(r, fd, POLLIN, quietEnd < deadline ? quietEnd : deadline)) {
            CloseSocket(fd);
            const bool quiet = quietEnd < deadline && MonoMs() >= quietEnd;
            Fail(r, quiet ? "The server sent nothing for too long." : "The server did not answer in time.");
            return;
        }
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n == 0) closed = true;
        else if (n < 0) { if (errno != EAGAIN && errno != EINTR) closed = true; }
        else { in.append(buf, size_t(n)); lastByte = MonoMs(); }
    }
    CloseSocket(fd);
    if (headEnd == std::string::npos || in.compare(0, 5, "HTTP/") != 0) { Fail(r, "The server's answer was not understood."); return; }
    const size_t sp = in.find(' ');
    r->res.status = sp == std::string::npos ? 0 : atoi(in.c_str() + sp + 1);
    std::string body = in.substr(headEnd);
    if (chunked) {
        std::string out;
        size_t p = 0;
        while (p < body.size()) {
            const size_t e = body.find("\r\n", p);
            if (e == std::string::npos) break;
            const long len = strtol(body.c_str() + p, 0, 16);
            if (len <= 0) break;
            p = e + 2;
            if (p + size_t(len) > body.size()) break;
            out.append(body, p, size_t(len));
            p += size_t(len) + 2;
        }
        body.swap(out);
    } else if (contentLength >= 0 && body.size() > size_t(contentLength)) {
        body.resize(size_t(contentLength));
    }
    r->res.body.swap(body);
}

void* Thread(void* arg) {
    Req* r = static_cast<Req*>(arg);
    Run(r);
    int expected = 0;
    if (!__atomic_compare_exchange_n(&r->state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) delete r;   // abandoned
    return 0;
}

}  // namespace

// Called by np_http.cpp for http:// URLs. Returns an opaque request or null.
void HttpTestSetDefaultIoMs(uint32_t ms) { g_defaultIoMs = ms; }
int HttpTestPlainLive() { return __atomic_load_n(&g_live, __ATOMIC_RELAXED); }

void* PlainStart(const char* method, const std::string& url, const std::string& body, const std::vector<std::string>& headers, uint32_t timeoutMs,
                 uint32_t ioTimeoutMs, std::string* err) {
    if (url.compare(0, 7, "http://") != 0) { *err = "Not an http URL."; return 0; }
    Req* r = new Req;
    std::string rest = url.substr(7);
    const size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    r->path = slash == std::string::npos ? "/" : rest.substr(slash);
    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        r->port = uint16_t(atoi(hostport.c_str() + colon + 1));
        hostport.resize(colon);
    }
    r->host = hostport;
    r->method = method;
    r->body = body;
    for (size_t i = 0; i < headers.size(); ++i) r->extra += headers[i] + "\r\n";
    r->timeoutMs = timeoutMs ? timeoutMs : 10000;
    r->ioTimeoutMs = ioTimeoutMs ? ioTimeoutMs : DefaultIoMs();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128 * 1024);
    pthread_t t;
    const int rc = pthread_create(&t, &attr, Thread, r);
    pthread_attr_destroy(&attr);
    if (rc != 0) { delete r; *err = "Could not start the request."; return 0; }
    pthread_detach(t);
    return r;
}

int PlainPoll(void* request, HttpResponse* out) {
    Req* r = static_cast<Req*>(request);
    if (__atomic_load_n(&r->state, __ATOMIC_ACQUIRE) != 1) return 0;
    *out = r->res;
    delete r;
    return 1;
}

void PlainCancel(void* request) {
    Req* r = static_cast<Req*>(request);
    int expected = 0;
    if (!__atomic_compare_exchange_n(&r->state, &expected, 2, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) delete r;   // it had finished
}

}  // namespace NpNet

#endif  // !NP_TARGET_VITA
