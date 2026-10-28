// HTTPS GET (updater.h http_get): WinHTTP on Windows (part of the system); on Linux the system's libcurl, opened at run
// time (libcurl.so.4: SteamOS and the desktop distributions have it), so the updater starts without it and says so.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "updater.h"

#define USER_AGENT "MHGURecomp-updater/" MHGU_VERSION
#define GITHUB_JSON "application/vnd.github+json"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = calloc(n ? n : 1, sizeof *w);
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static void win_error(HttpResult *r, const char *what) {
    DWORD e = GetLastError();
    const char *why = e == ERROR_WINHTTP_TIMEOUT ? "timed out"
                    : e == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "no connection (name not resolved)"
                    : e == ERROR_WINHTTP_CANNOT_CONNECT ? "no connection"
                    : e == ERROR_WINHTTP_SECURE_FAILURE ? "secure connection failed" : NULL;
    if (why) snprintf(r->error, sizeof r->error, "%s: %s", what, why);
    else snprintf(r->error, sizeof r->error, "%s: error %lu", what, e);
}

bool http_get(const char *url, const char *accept, const char *if_none_match, int connect_timeout, HttpSink sink,
              void *ctx, HttpResult *r) {
    memset(r, 0, sizeof *r);
    r->rate_remaining = -1;
    bool ok = false;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    wchar_t *wurl = widen(url), *wagent = widen(USER_AGENT), *wheaders = NULL;
    wchar_t host[256], path[2048], extra[2048];
    URL_COMPONENTS u = {sizeof u};
    u.lpszHostName = host, u.dwHostNameLength = 256;
    u.lpszUrlPath = path, u.dwUrlPathLength = 2048;
    u.lpszExtraInfo = extra, u.dwExtraInfoLength = 2048;
    if (!wurl || !wagent || !WinHttpCrackUrl(wurl, 0, 0, &u)) { snprintf(r->error, sizeof r->error, "bad URL"); goto out; }
    wcsncat(path, extra, 2047 - wcslen(path));
    ses = WinHttpOpen(wagent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)  // (before Windows 8.1)
        ses = WinHttpOpen(wagent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { win_error(r, "WinHTTP"); goto out; }
    int ms = connect_timeout * 1000;
    WinHttpSetTimeouts(ses, ms, ms, ms, ms);
    if (!(con = WinHttpConnect(ses, host, u.nPort, 0))) { win_error(r, "connect"); goto out; }
    req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { win_error(r, "request"); goto out; }
    char headers[512];
    snprintf(headers, sizeof headers, "Accept: %s\r\n%s%s%s%s", accept,
             !strcmp(accept, GITHUB_JSON) ? "X-GitHub-Api-Version: 2022-11-28\r\n" : "",
             if_none_match ? "If-None-Match: " : "", if_none_match ? if_none_match : "", if_none_match ? "\r\n" : "");
    if (!(wheaders = widen(headers))) goto out;
    if (!WinHttpSendRequest(req, wheaders, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        win_error(r, "no answer");
        goto out;
    }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    r->status = (int)status;
    wchar_t wval[128];
    size = sizeof wval;
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX, wval, &size, WINHTTP_NO_HEADER_INDEX))
        WideCharToMultiByte(CP_UTF8, 0, wval, -1, r->etag, sizeof r->etag, NULL, NULL);
    size = sizeof wval;
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"X-RateLimit-Remaining", wval, &size, WINHTTP_NO_HEADER_INDEX))
        r->rate_remaining = wcstol(wval, NULL, 10);
    if (status != 200) { ok = true; goto out; }
    static char buf[1 << 16];
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(req, buf, sizeof buf, &got)) { win_error(r, "download"); goto out; }
        if (!got) break;
        if (!sink(ctx, buf, got)) { snprintf(r->error, sizeof r->error, "stopped"); goto out; }
    }
    ok = true;
out:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    free(wurl), free(wagent), free(wheaders);
    return ok;
}

#else
#include <curl/curl.h>
#include <dlfcn.h>
#include <unistd.h>

static struct {
    CURLcode (*global_init)(long);
    CURL *(*init)(void);
    CURLcode (*setopt)(CURL *, CURLoption, ...);
    CURLcode (*perform)(CURL *);
    void (*cleanup)(CURL *);
    struct curl_slist *(*append)(struct curl_slist *, const char *);
    void (*free_all)(struct curl_slist *);
    const char *(*strerror)(CURLcode);
} C;

static bool load_curl(char *error, size_t size) {
    static int loaded;  // 1 yes, -1 no
    if (loaded) return loaded > 0 || (snprintf(error, size, "libcurl not found (install curl)"), false);
    const char *names[] = {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"};
    void *h = NULL;
    for (size_t i = 0; !h && i < sizeof names / sizeof *names; i++) h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    loaded = -1;
    if (!h) return load_curl(error, size);
    *(void **)&C.global_init = dlsym(h, "curl_global_init");
    *(void **)&C.init = dlsym(h, "curl_easy_init");
    *(void **)&C.setopt = dlsym(h, "curl_easy_setopt");
    *(void **)&C.perform = dlsym(h, "curl_easy_perform");
    *(void **)&C.cleanup = dlsym(h, "curl_easy_cleanup");
    *(void **)&C.append = dlsym(h, "curl_slist_append");
    *(void **)&C.free_all = dlsym(h, "curl_slist_free_all");
    *(void **)&C.strerror = dlsym(h, "curl_easy_strerror");
    if (!C.global_init || !C.init || !C.setopt || !C.perform || !C.cleanup || !C.append || !C.free_all || !C.strerror)
        return load_curl(error, size);
    C.global_init(CURL_GLOBAL_DEFAULT);
    loaded = 1;
    return true;
}

typedef struct { HttpResult *r; HttpSink sink; void *ctx; bool stopped; } Transfer;

static size_t on_header(char *data, size_t one, size_t n, void *arg) {
    Transfer *t = arg;
    n *= one;
    char line[256];
    snprintf(line, sizeof line, "%.*s", (int)(n < 255 ? n : 255), data);
    line[strcspn(line, "\r\n")] = 0;
    int status;
    if (!strncmp(line, "HTTP/", 5) && sscanf(line, "HTTP/%*s %d", &status) == 1) {  // (each answer: a redirect's too)
        t->r->status = status, t->r->rate_remaining = -1, t->r->etag[0] = 0;
    } else if (!strncasecmp(line, "etag:", 5)) {
        snprintf(t->r->etag, sizeof t->r->etag, "%s", line + 5 + strspn(line + 5, " "));
    } else if (!strncasecmp(line, "x-ratelimit-remaining:", 22)) {
        t->r->rate_remaining = strtol(line + 22, NULL, 10);
    }
    return n;
}

static size_t on_data(char *data, size_t one, size_t n, void *arg) {
    Transfer *t = arg;
    if (t->r->status != 200) return one * n;  // (an error's body)
    if (!t->sink(t->ctx, data, one * n)) { t->stopped = true; return 0; }
    return one * n;
}

bool http_get(const char *url, const char *accept, const char *if_none_match, int connect_timeout, HttpSink sink,
              void *ctx, HttpResult *r) {
    memset(r, 0, sizeof *r);
    r->rate_remaining = -1;
    if (!load_curl(r->error, sizeof r->error)) return false;
    CURL *c = C.init();
    if (!c) { snprintf(r->error, sizeof r->error, "libcurl failed"); return false; }
    char hdr[512], errbuf[CURL_ERROR_SIZE] = "";
    struct curl_slist *list = NULL;
    snprintf(hdr, sizeof hdr, "Accept: %s", accept);
    list = C.append(list, hdr);
    if (!strcmp(accept, GITHUB_JSON)) list = C.append(list, "X-GitHub-Api-Version: 2022-11-28");
    if (if_none_match) {
        snprintf(hdr, sizeof hdr, "If-None-Match: %s", if_none_match);
        list = C.append(list, hdr);
    }
    Transfer t = {r, sink, ctx, false};
    C.setopt(c, CURLOPT_URL, url);
    C.setopt(c, CURLOPT_USERAGENT, USER_AGENT);
    C.setopt(c, CURLOPT_HTTPHEADER, list);
    C.setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    C.setopt(c, CURLOPT_MAXREDIRS, 5L);
    C.setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    C.setopt(c, CURLOPT_CONNECTTIMEOUT, (long)connect_timeout);
    C.setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);  // (stalled for connect_timeout seconds: give up)
    C.setopt(c, CURLOPT_LOW_SPEED_TIME, (long)connect_timeout);
    C.setopt(c, CURLOPT_NOSIGNAL, 1L);
    C.setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    C.setopt(c, CURLOPT_HEADERFUNCTION, on_header);
    C.setopt(c, CURLOPT_HEADERDATA, &t);
    C.setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    C.setopt(c, CURLOPT_WRITEDATA, &t);
    CURLcode e = C.perform(c);
    if (e == CURLE_PEER_FAILED_VERIFICATION || e == CURLE_SSL_CACERT_BADFILE) {
        // (a libcurl built for another system's certificate store, e.g. a runtime's: the usual places of the bundle)
        const char *bundles[] = {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                                 "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem"};
        for (size_t i = 0; i < sizeof bundles / sizeof *bundles; i++)
            if (!access(bundles[i], R_OK)) {
                C.setopt(c, CURLOPT_CAINFO, bundles[i]);
                r->status = 0;
                e = C.perform(c);
                break;
            }
    }
    C.cleanup(c);
    C.free_all(list);
    if (e == CURLE_OK) return true;
    if (t.stopped) snprintf(r->error, sizeof r->error, "stopped");
    else snprintf(r->error, sizeof r->error, "%s", errbuf[0] ? errbuf : C.strerror(e));
    r->status = 0;
    return false;
}
#endif
