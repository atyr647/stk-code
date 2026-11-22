/* The libcurl API subset STK uses (src/online/http_request_curl.cpp), for the
 * N64: a valid handle whose every transfer fails with "couldn't connect",
 * which STK already reports as an offline error. */
#ifndef STK_N64_CURL_H
#define STK_N64_CURL_H
#include <stddef.h>
#include <stdint.h>
#include "curlver.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef void CURL;
typedef int64_t curl_off_t;
typedef enum {
    CURLE_OK = 0, CURLE_FAILED_INIT = 2, CURLE_COULDNT_RESOLVE_HOST = 6,
    CURLE_COULDNT_CONNECT = 7, CURLE_WRITE_ERROR = 23,
    CURLE_ABORTED_BY_CALLBACK = 42
} CURLcode;
typedef enum {
    CURLOPT_WRITEDATA = 10001, CURLOPT_URL = 10002, CURLOPT_POSTFIELDS = 10015,
    CURLOPT_USERAGENT = 10018, CURLOPT_HTTPHEADER = 10023,
    CURLOPT_CAINFO = 10065, CURLOPT_PROGRESSDATA = 10057,
    CURLOPT_XFERINFODATA = 10057, CURLOPT_WRITEFUNCTION = 20011,
    CURLOPT_PROGRESSFUNCTION = 20056, CURLOPT_XFERINFOFUNCTION = 20219,
    CURLOPT_LOW_SPEED_LIMIT = 19, CURLOPT_LOW_SPEED_TIME = 20,
    CURLOPT_VERBOSE = 41, CURLOPT_NOPROGRESS = 43, CURLOPT_FOLLOWLOCATION = 52,
    CURLOPT_SSL_VERIFYPEER = 64, CURLOPT_CONNECTTIMEOUT = 78,
    CURLOPT_SSL_VERIFYHOST = 81, CURLOPT_NOSIGNAL = 99
} CURLoption;
#define CURL_GLOBAL_DEFAULT 3
#define CURL_ERROR_SIZE 256
struct curl_slist { char *data; struct curl_slist *next; };
CURLcode curl_global_init(long flags);
void curl_global_cleanup(void);
CURL *curl_easy_init(void);
CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...);
CURLcode curl_easy_perform(CURL *curl);
void curl_easy_cleanup(CURL *curl);
const char *curl_easy_strerror(CURLcode code);
struct curl_slist *curl_slist_append(struct curl_slist *list, const char *s);
void curl_slist_free_all(struct curl_slist *list);
#ifdef __cplusplus
}
#endif
#endif
