/*
 * This file Copyright (C) 2008-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <algorithm>
#include <cstring> /* strlen(), strstr() */
#include <set>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

#include <curl/curl.h>

#include <event2/buffer.h>

#include "transmission.h"
#include "crypto-utils.h"
#include "file.h"
#include "log.h"
#include "net.h" /* tr_address */
#include "torrent.h"
#include "platform.h" /* mutex */
#include "session.h"
#include "tr-assert.h"
#include "tr-macros.h"
#include "trevent.h" /* tr_runInEventThread() */
#include "utils.h"
#include "version.h" /* User-Agent */
#include "web.h"

#if LIBCURL_VERSION_NUM >= 0x070F06 /* CURLOPT_SOCKOPT* was added in 7.15.6 */
#define USE_LIBCURL_SOCKOPT
#endif

enum
{
    THREADFUNC_MAX_SLEEP_MSEC = 200,
};

#if 0
#define dbgmsg(fmt, ...) fprintf(stderr, fmt "\n", __VA_ARGS__)
#else
#define dbgmsg(...) tr_logAddDeepNamed("web", __VA_ARGS__)
#endif

/***
****
***/

struct tr_web_task
{
    int torrentId;
    long code;
    long timeout_secs;
    bool did_connect;
    bool did_timeout;
    struct evbuffer* response;
    struct evbuffer* freebuf;
    char* url;
    char* range;
    char* cookies;
    tr_session* session;
    tr_web_done_func done_func;
    void* done_func_user_data;
    CURL* curl_easy;
    struct tr_web_task* next;
};

static void task_free(struct tr_web_task* task)
{
    if (task->freebuf != nullptr)
    {
        evbuffer_free(task->freebuf);
    }

    tr_free(task->cookies);
    tr_free(task->range);
    tr_free(task->url);
    tr_free(task);
}

/***
****
***/

struct tr_web
{
    bool curl_verbose;
    bool curl_ssl_verify;
    char* curl_ca_bundle;
    int close_mode;
    struct tr_web_task* tasks;
    tr_lock* taskLock;
    char* cookie_filename;
    std::set<CURL*> paused_easy_handles;
};

/***
****
***/

static size_t writeFunc(void* ptr, size_t size, size_t nmemb, void* vtask)
{
    size_t const byteCount = size * nmemb;
    auto* task = static_cast<struct tr_web_task*>(vtask);

    /* webseed downloads should be speed limited */
    if (task->torrentId != -1)
    {
        tr_torrent const* const tor = tr_torrentFindFromId(task->session, task->torrentId);

        if (tor != nullptr && tor->bandwidth->clamp(TR_DOWN, nmemb) == 0)
        {
            task->session->web->paused_easy_handles.insert(task->curl_easy);
            return CURL_WRITEFUNC_PAUSE;
        }
    }

    evbuffer_add(task->response, ptr, byteCount);
    dbgmsg("wrote %zu bytes to task %p's buffer", byteCount, (void*)task);
    return byteCount;
}

#ifdef USE_LIBCURL_SOCKOPT

static int sockoptfunction(void* vtask, curl_socket_t fd, [[maybe_unused]] curlsocktype purpose)
{
    auto* task = static_cast<struct tr_web_task*>(vtask);
    bool const isScrape = strstr(task->url, "scrape") != nullptr;
    bool const isAnnounce = strstr(task->url, "announce") != nullptr;

    /* announce and scrape requests have tiny payloads. */
    if (isScrape || isAnnounce)
    {
        int const sndbuf = isScrape ? 4096 : 1024;
        int const rcvbuf = isScrape ? 4096 : 3072;
        /* ignore the sockopt() return values -- these are suggestions
           rather than hard requirements & it's OK for them to fail */
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char const*>(&sndbuf), sizeof(sndbuf));
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char const*>(&rcvbuf), sizeof(rcvbuf));
    }

    /* return nonzero if this function encountered an error */
    return 0;
}

#endif

static CURLcode ssl_context_func([[maybe_unused]] CURL* curl, void* ssl_ctx, [[maybe_unused]] void* user_data)
{
    tr_x509_store_t const cert_store = tr_ssl_get_x509_store(ssl_ctx);
    if (cert_store == nullptr)
    {
        return CURLE_OK;
    }

#ifdef _WIN32

    curl_version_info_data const* const curl_ver = curl_version_info(CURLVERSION_NOW);
    if (curl_ver->age >= 0 && strncmp(curl_ver->ssl_version, "Schannel", 8) == 0)
    {
        return CURLE_OK;
    }

    static LPCWSTR const sys_store_names[] = {
        L"CA",
        L"ROOT",
    };

    for (size_t i = 0; i < TR_N_ELEMENTS(sys_store_names); ++i)
    {
        HCERTSTORE const sys_cert_store = CertOpenSystemStoreW(0, sys_store_names[i]);
        if (sys_cert_store == nullptr)
        {
            continue;
        }

        PCCERT_CONTEXT sys_cert = nullptr;

        while (true)
        {
            sys_cert = CertFindCertificateInStore(sys_cert_store, X509_ASN_ENCODING, 0, CERT_FIND_ANY, nullptr, sys_cert);
            if (sys_cert == nullptr)
            {
                break;
            }

            tr_x509_cert_t const cert = tr_x509_cert_new(sys_cert->pbCertEncoded, sys_cert->cbCertEncoded);
            if (cert == nullptr)
            {
                continue;
            }

            tr_x509_store_add(cert_store, cert);
            tr_x509_cert_free(cert);
        }

        CertCloseStore(sys_cert_store, 0);
    }

#endif

    return CURLE_OK;
}

static long getTimeoutFromURL(struct tr_web_task const* task)
{
    long timeout;
    tr_session const* session = task->session;

    if (session == nullptr || session->isClosed)
    {
        timeout = 20L;
    }
    else if (strstr(task->url, "scrape") != nullptr)
    {
        timeout = 30L;
    }
    else if (strstr(task->url, "announce") != nullptr)
    {
        timeout = 90L;
    }
    else
    {
        timeout = 240L;
    }

    return timeout;
}

static CURL* createEasy(tr_session* s, struct tr_web* web, struct tr_web_task* task)
{
    bool is_default_value;
    tr_address const* addr;
    CURL* e = curl_easy_init();

    task->curl_easy = e;
    task->timeout_secs = getTimeoutFromURL(task);

    curl_easy_setopt(e, CURLOPT_AUTOREFERER, 1L);
    curl_easy_setopt(e, CURLOPT_ENCODING, "");
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_MAXREDIRS, -1L);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_PRIVATE, task);

#ifdef USE_LIBCURL_SOCKOPT
    curl_easy_setopt(e, CURLOPT_SOCKOPTFUNCTION, sockoptfunction);
    curl_easy_setopt(e, CURLOPT_SOCKOPTDATA, task);
#endif

    if (web->curl_ssl_verify)
    {
        if (web->curl_ca_bundle != nullptr)
        {
            curl_easy_setopt(e, CURLOPT_CAINFO, web->curl_ca_bundle);
        }
        else
        {
            curl_easy_setopt(e, CURLOPT_SSL_CTX_FUNCTION, ssl_context_func);
        }
    }
    else
    {
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 0L);
    }

    curl_easy_setopt(e, CURLOPT_TIMEOUT, task->timeout_secs);
    curl_easy_setopt(e, CURLOPT_URL, task->url);
    curl_easy_setopt(e, CURLOPT_USERAGENT, TR_NAME "/" SHORT_VERSION_STRING);
    curl_easy_setopt(e, CURLOPT_VERBOSE, (long)(web->curl_verbose ? 1 : 0));
    curl_easy_setopt(e, CURLOPT_WRITEDATA, task);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, writeFunc);

    if ((addr = tr_sessionGetPublicAddress(s, TR_AF_INET, &is_default_value)) != nullptr && !is_default_value)
    {
        curl_easy_setopt(e, CURLOPT_INTERFACE, tr_address_to_string(addr));
    }
    else if ((addr = tr_sessionGetPublicAddress(s, TR_AF_INET6, &is_default_value)) != nullptr && !is_default_value)
    {
        curl_easy_setopt(e, CURLOPT_INTERFACE, tr_address_to_string(addr));
    }

    if (task->cookies != nullptr)
    {
        curl_easy_setopt(e, CURLOPT_COOKIE, task->cookies);
    }

    if (web->cookie_filename != nullptr)
    {
        curl_easy_setopt(e, CURLOPT_COOKIEFILE, web->cookie_filename);
    }

    if (task->range != nullptr)
    {
        curl_easy_setopt(e, CURLOPT_RANGE, task->range);
        /* don't bother asking the server to compress webseed fragments */
        curl_easy_setopt(e, CURLOPT_ENCODING, "identity");
    }

    return e;
}

/***
****
***/

static void task_finish_func(void* vtask)
{
    auto* task = static_cast<struct tr_web_task*>(vtask);
    dbgmsg("finished web task %p; got %ld", (void*)task, task->code);

    if (task->done_func != nullptr)
    {
        (*task->done_func)(
            task->session,
            task->did_connect,
            task->did_timeout,
            task->code,
            evbuffer_pullup(task->response, -1),
            evbuffer_get_length(task->response),
            task->done_func_user_data);
    }

    task_free(task);
}

/****
*****
****/

static void tr_webThreadFunc(void* vsession);

static struct tr_web_task* tr_webRunImpl(
    tr_session* session,
    int torrentId,
    char const* url,
    char const* range,
    char const* cookies,
    tr_web_done_func done_func,
    void* done_func_user_data,
    struct evbuffer* buffer)
{
    struct tr_web_task* task = nullptr;

    if (!session->isClosing)
    {
        if (session->web == nullptr)
        {
            tr_threadNew(tr_webThreadFunc, session);

            while (session->web == nullptr)
            {
                tr_wait_msec(20);
            }
        }

        task = tr_new0(struct tr_web_task, 1);
        task->session = session;
        task->torrentId = torrentId;
        task->url = tr_strdup(url);
        task->range = tr_strdup(range);
        task->cookies = tr_strdup(cookies);
        task->done_func = done_func;
        task->done_func_user_data = done_func_user_data;
        task->response = buffer != nullptr ? buffer : evbuffer_new();
        task->freebuf = buffer != nullptr ? nullptr : task->response;

        tr_lockLock(session->web->taskLock);
        task->next = session->web->tasks;
        session->web->tasks = task;
        tr_lockUnlock(session->web->taskLock);
    }

    return task;
}

struct tr_web_task* tr_webRunWithCookies(
    tr_session* session,
    char const* url,
    char const* cookies,
    tr_web_done_func done_func,
    void* done_func_user_data)
{
    return tr_webRunImpl(session, -1, url, nullptr, cookies, done_func, done_func_user_data, nullptr);
}

struct tr_web_task* tr_webRun(tr_session* session, char const* url, tr_web_done_func done_func, void* done_func_user_data)
{
    return tr_webRunWithCookies(session, url, nullptr, done_func, done_func_user_data);
}

struct tr_web_task* tr_webRunWebseed(
    tr_torrent* tor,
    char const* url,
    char const* range,
    tr_web_done_func done_func,
    void* done_func_user_data,
    struct evbuffer* buffer)
{
    return tr_webRunImpl(tor->session, tr_torrentId(tor), url, range, nullptr, done_func, done_func_user_data, buffer);
}

static void tr_webThreadFunc(void* vsession)
{
    char* str;
    CURLM* multi;
    int taskCount = 0;
    uint32_t repeats = 0;
    auto* session = static_cast<tr_session*>(vsession);

    /* try to enable ssl for https support; but if that fails,
     * try a plain vanilla init */
    if (curl_global_init(CURL_GLOBAL_SSL) != CURLE_OK)
    {
        curl_global_init(0);
    }

    auto* web = new tr_web{};
    web->close_mode = ~0;
    web->taskLock = tr_lockNew();
    web->tasks = nullptr;
    web->curl_verbose = tr_env_key_exists("TR_CURL_VERBOSE");
    web->curl_ssl_verify = !tr_env_key_exists("TR_CURL_SSL_NO_VERIFY");
    web->curl_ca_bundle = tr_env_get_string("CURL_CA_BUNDLE", nullptr);

    if (web->curl_ssl_verify)
    {
        tr_logAddNamedInfo(
            "web",
            "will verify tracker certs using envvar CURL_CA_BUNDLE: %s",
            web->curl_ca_bundle == nullptr ? "none" : web->curl_ca_bundle);
        tr_logAddNamedInfo("web", "NB: this only works if you built against libcurl with openssl or gnutls, NOT nss");
        tr_logAddNamedInfo("web", "NB: invalid certs will show up as 'Could not connect to tracker' like many other errors");
    }

    str = tr_buildPath(session->configDir, "cookies.txt", nullptr);

    if (tr_sys_path_exists(str, nullptr))
    {
        web->cookie_filename = tr_strdup(str);
    }

    tr_free(str);

    multi = curl_multi_init();
    session->web = web;

    for (;;)
    {
        long msec;
        int numfds;
        int unused;
        CURLMsg* msg;
        CURLMcode mcode;

        if (web->close_mode == TR_WEB_CLOSE_NOW)
        {
            break;
        }

        if (web->close_mode == TR_WEB_CLOSE_WHEN_IDLE && web->tasks == nullptr)
        {
            break;
        }

        /* add tasks from the queue */
        tr_lockLock(web->taskLock);

        while (web->tasks != nullptr)
        {
            /* pop the task */
            struct tr_web_task* task = web->tasks;
            web->tasks = task->next;
            task->next = nullptr;

            dbgmsg("adding task to curl: [%s]", task->url);
            curl_multi_add_handle(multi, createEasy(session, web, task));
            ++taskCount;
        }

        tr_lockUnlock(web->taskLock);

        /* resume any paused curl handles.
           swap paused_easy_handles to prevent oscillation
           between writeFunc this while loop */
        auto paused = decltype(web->paused_easy_handles){};
        std::swap(paused, web->paused_easy_handles);
        std::for_each(std::begin(paused), std::end(paused), [](auto* curl) { curl_easy_pause(curl, CURLPAUSE_CONT); });

        /* maybe wait a little while before calling curl_multi_perform() */
        msec = 0;
        curl_multi_timeout(multi, &msec);

        if (msec < 0)
        {
            msec = THREADFUNC_MAX_SLEEP_MSEC;
        }

        if (session->isClosed)
        {
            msec = 100; /* on shutdown, call perform() more frequently */
        }

        if (msec > 0)
        {
            if (msec > THREADFUNC_MAX_SLEEP_MSEC)
            {
                msec = THREADFUNC_MAX_SLEEP_MSEC;
            }

            curl_multi_wait(multi, nullptr, 0, msec, &numfds);
            if (!numfds)
            {
                repeats++;
                if (repeats > 1)
                {
                    /* curl_multi_wait() returns immediately if there are
                     * no fds to wait for, so we need an explicit wait here
                     * to emulate select() behavior */
                    tr_wait_msec(std::min(msec, THREADFUNC_MAX_SLEEP_MSEC / 2L));
                }
            }
            else
            {
                repeats = 0;
            }
        }

        /* call curl_multi_perform() */
        do
        {
            mcode = curl_multi_perform(multi, &unused);
        } while (mcode == CURLM_CALL_MULTI_PERFORM);

        /* pump completed tasks from the multi */
        while ((msg = curl_multi_info_read(multi, &unused)) != nullptr)
        {
            if (msg->msg == CURLMSG_DONE && msg->easy_handle != nullptr)
            {
                double total_time;
                struct tr_web_task* task;
                long req_bytes_sent;
                CURL* e = msg->easy_handle;
                curl_easy_getinfo(e, CURLINFO_PRIVATE, (void*)&task);

                TR_ASSERT(e == task->curl_easy);

                curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &task->code);
                curl_easy_getinfo(e, CURLINFO_REQUEST_SIZE, &req_bytes_sent);
                curl_easy_getinfo(e, CURLINFO_TOTAL_TIME, &total_time);
                task->did_connect = task->code > 0 || req_bytes_sent > 0;
                task->did_timeout = task->code == 0 && total_time >= task->timeout_secs;
                curl_multi_remove_handle(multi, e);
                web->paused_easy_handles.erase(e);
                curl_easy_cleanup(e);
                tr_runInEventThread(task->session, task_finish_func, task);
                --taskCount;
            }
        }
    }

    /* Discard any remaining tasks.
     * This is rare, but can happen on shutdown with unresponsive trackers. */
    while (web->tasks != nullptr)
    {
        struct tr_web_task* task = web->tasks;
        web->tasks = task->next;
        dbgmsg("Discarding task \"%s\"", task->url);
        task_free(task);
    }

    /* cleanup */
    curl_multi_cleanup(multi);
    tr_lockFree(web->taskLock);
    tr_free(web->curl_ca_bundle);
    tr_free(web->cookie_filename);
    delete web;
    session->web = nullptr;
}

void tr_webClose(tr_session* session, tr_web_close_mode close_mode)
{
    if (session->web != nullptr)
    {
        session->web->close_mode = close_mode;

        if (close_mode == TR_WEB_CLOSE_NOW)
        {
            while (session->web != nullptr)
            {
                tr_wait_msec(100);
            }
        }
    }
}

long tr_webGetTaskResponseCode(struct tr_web_task* task)
{
    long code = 0;
    curl_easy_getinfo(task->curl_easy, CURLINFO_RESPONSE_CODE, &code);
    return code;
}

char const* tr_webGetTaskRealUrl(struct tr_web_task* task)
{
    char* url = nullptr;
    curl_easy_getinfo(task->curl_easy, CURLINFO_EFFECTIVE_URL, &url);
    return url;
}

/*****
******
******
*****/

char const* tr_webGetResponseStr(long code)
{
    switch (code)
    {
    case 0:
        return "No Response";

    case 101:
        return "Switching Protocols";

    case 200:
        return "OK";

    case 201:
        return "Created";

    case 202:
        return "Accepted";

    case 203:
        return "Non-Authoritative Information";

    case 204:
        return "No Content";

    case 205:
        return "Reset Content";

    case 206:
        return "Partial Content";

    case 300:
        return "Multiple Choices";

    case 301:
        return "Moved Permanently";

    case 302:
        return "Found";

    case 303:
        return "See Other";

    case 304:
        return "Not Modified";

    case 305:
        return "Use Proxy";

    case 306:
        return " (Unused)";

    case 307:
        return "Temporary Redirect";

    case 400:
        return "Bad Request";

    case 401:
        return "Unauthorized";

    case 402:
        return "Payment Required";

    case 403:
        return "Forbidden";

    case 404:
        return "Not Found";

    case 405:
        return "Method Not Allowed";

    case 406:
        return "Not Acceptable";

    case 407:
        return "Proxy Authentication Required";

    case 408:
        return "Request Timeout";

    case 409:
        return "Conflict";

    case 410:
        return "Gone";

    case 411:
        return "Length Required";

    case 412:
        return "Precondition Failed";

    case 413:
        return "Request Entity Too Large";

    case 414:
        return "Request-URI Too Long";

    case 415:
        return "Unsupported Media Type";

    case 416:
        return "Requested Range Not Satisfiable";

    case 417:
        return "Expectation Failed";

    case 421:
        return "Misdirected Request";

    case 500:
        return "Internal Server Error";

    case 501:
        return "Not Implemented";

    case 502:
        return "Bad Gateway";

    case 503:
        return "Service Unavailable";

    case 504:
        return "Gateway Timeout";

    case 505:
        return "HTTP Version Not Supported";

    default:
        return "Unknown Error";
    }
}

void tr_http_escape(struct evbuffer* out, char const* str, size_t len, bool escape_slashes)
{
    if (str == nullptr)
    {
        return;
    }

    if (len == TR_BAD_SIZE)
    {
        len = strlen(str);
    }

    for (char const* end = str + len; str != end; ++str)
    {
        if (*str == ',' || *str == '-' || *str == '.' || ('0' <= *str && *str <= '9') || ('A' <= *str && *str <= 'Z') ||
            ('a' <= *str && *str <= 'z') || (*str == '/' && !escape_slashes))
        {
            evbuffer_add_printf(out, "%c", *str);
        }
        else
        {
            evbuffer_add_printf(out, "%%%02X", (unsigned)(*str & 0xFF));
        }
    }
}

char* tr_http_unescape(char const* str, size_t len)
{
    char* tmp = curl_unescape(str, len);
    char* ret = tr_strdup(tmp);
    curl_free(tmp);
    return ret;
}

static bool is_rfc2396_alnum(uint8_t ch)
{
    return ('0' <= ch && ch <= '9') || ('A' <= ch && ch <= 'Z') || ('a' <= ch && ch <= 'z') || ch == '.' || ch == '-' ||
        ch == '_' || ch == '~';
}

void tr_http_escape_sha1(char* out, uint8_t const* sha1_digest)
{
    uint8_t const* in = sha1_digest;
    uint8_t const* end = in + SHA_DIGEST_LENGTH;

    while (in != end)
    {
        if (is_rfc2396_alnum(*in))
        {
            *out++ = (char)*in++;
        }
        else
        {
            out += tr_snprintf(out, 4, "%%%02x", (unsigned int)*in++);
        }
    }

    *out = '\0';
}
