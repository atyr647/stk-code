/* libcurl stub for the N64: see include/curl/curl.h */
#include <curl/curl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static int handle_token;
CURLcode curl_global_init(long flags) { (void)flags; return CURLE_OK; }
void curl_global_cleanup(void) {}
CURL *curl_easy_init(void) { return &handle_token; }
CURLcode curl_easy_setopt(CURL *c, CURLoption o, ...) { (void)c; (void)o; return CURLE_OK; }
CURLcode curl_easy_perform(CURL *c) { (void)c; return CURLE_COULDNT_CONNECT; }
void curl_easy_cleanup(CURL *c) { (void)c; }
const char *curl_easy_strerror(CURLcode code)
{ return code == CURLE_OK ? "No error" : "No network on the Nintendo 64"; }
struct curl_slist *curl_slist_append(struct curl_slist *list, const char *s)
{
    struct curl_slist *n = malloc(sizeof *n);
    if (!n) return NULL;
    n->data = strdup(s); n->next = NULL;
    if (!list) return n;
    struct curl_slist *l = list;
    while (l->next) l = l->next;
    l->next = n;
    return list;
}
void curl_slist_free_all(struct curl_slist *list)
{
    while (list) { struct curl_slist *n = list->next; free(list->data); free(list); list = n; }
}
