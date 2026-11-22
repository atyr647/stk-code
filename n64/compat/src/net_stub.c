/* The N64's socket layer: present, and always down. See stk_n64_net.h. */
#include <stk_n64_net.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

#define DOWN(ret) do { errno = ENETDOWN; return ret; } while (0)

int socket(int d, int t, int p) { (void)d; (void)t; (void)p; DOWN(-1); }
int bind(int f, const struct sockaddr *a, socklen_t l) { (void)f; (void)a; (void)l; DOWN(-1); }
int listen(int f, int b) { (void)f; (void)b; DOWN(-1); }
int accept(int f, struct sockaddr *a, socklen_t *l) { (void)f; (void)a; (void)l; DOWN(-1); }
int connect(int f, const struct sockaddr *a, socklen_t l) { (void)f; (void)a; (void)l; DOWN(-1); }
ssize_t send(int f, const void *b, size_t l, int fl) { (void)f; (void)b; (void)l; (void)fl; DOWN(-1); }
ssize_t recv(int f, void *b, size_t l, int fl) { (void)f; (void)b; (void)l; (void)fl; DOWN(-1); }
ssize_t sendto(int f, const void *b, size_t l, int fl, const struct sockaddr *t, socklen_t tl)
{ (void)f; (void)b; (void)l; (void)fl; (void)t; (void)tl; DOWN(-1); }
ssize_t recvfrom(int f, void *b, size_t l, int fl, struct sockaddr *a, socklen_t *al)
{ (void)f; (void)b; (void)l; (void)fl; (void)a; (void)al; DOWN(-1); }
ssize_t sendmsg(int f, const struct msghdr *m, int fl) { (void)f; (void)m; (void)fl; DOWN(-1); }
ssize_t recvmsg(int f, struct msghdr *m, int fl) { (void)f; (void)m; (void)fl; DOWN(-1); }
int setsockopt(int f, int lv, int n, const void *v, socklen_t l) { (void)f; (void)lv; (void)n; (void)v; (void)l; DOWN(-1); }
int getsockopt(int f, int lv, int n, void *v, socklen_t *l) { (void)f; (void)lv; (void)n; (void)v; (void)l; DOWN(-1); }
int getsockname(int f, struct sockaddr *a, socklen_t *l) { (void)f; (void)a; (void)l; DOWN(-1); }
int getpeername(int f, struct sockaddr *a, socklen_t *l) { (void)f; (void)a; (void)l; DOWN(-1); }
int shutdown(int f, int h) { (void)f; (void)h; DOWN(-1); }
int poll(struct pollfd *fds, nfds_t n, int t)
{
    for (nfds_t i = 0; i < n; i++) fds[i].revents = POLLNVAL;
    (void)t; DOWN(-1);
}

int getaddrinfo(const char *n, const char *s, const struct addrinfo *h, struct addrinfo **r)
{ (void)n; (void)s; (void)h; if (r) *r = NULL; return EAI_FAIL; }
void freeaddrinfo(struct addrinfo *r) { (void)r; }
const char *gai_strerror(int e) { (void)e; return "no network on the Nintendo 64"; }
int getnameinfo(const struct sockaddr *sa, socklen_t sl, char *h, socklen_t hl,
                char *s, socklen_t svl, int f)
{ (void)sa; (void)sl; (void)h; (void)hl; (void)s; (void)svl; (void)f; return EAI_FAIL; }
struct hostent *gethostbyname(const char *n) { (void)n; return NULL; }
struct hostent *gethostbyaddr(const void *a, socklen_t l, int t) { (void)a; (void)l; (void)t; return NULL; }
int gethostname(char *name, size_t len)
{ if (len) { strncpy(name, "n64", len); name[len - 1] = 0; } return 0; }
struct servent *getservbyname(const char *n, const char *p) { (void)n; (void)p; return NULL; }
struct servent *getservbyport(int port, const char *p) { (void)port; (void)p; return NULL; }
struct protoent *getprotobynumber(int p) { (void)p; return NULL; }
struct protoent *getprotobyname(const char *n) { (void)n; return NULL; }
int getifaddrs(struct ifaddrs **i) { if (i) *i = NULL; DOWN(-1); }
void freeifaddrs(struct ifaddrs *i) { (void)i; }
unsigned int if_nametoindex(const char *n) { (void)n; return 0; }

/* Address text conversion is pure computation, so it is real. */
int inet_pton(int af, const char *src, void *dst)
{
    if (af == AF_INET) {
        unsigned a, b, c, d; char tail;
        if (sscanf(src, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 ||
            a > 255 || b > 255 || c > 255 || d > 255)
            return 0;
        uint8_t *o = dst; o[0] = a; o[1] = b; o[2] = c; o[3] = d;
        return 1;
    }
    if (af == AF_INET6) {
        /* only the forms STK writes itself: "::" and "::1" */
        uint8_t *o = dst; memset(o, 0, 16);
        if (!strcmp(src, "::")) return 1;
        if (!strcmp(src, "::1")) { o[15] = 1; return 1; }
        return 0;
    }
    errno = EAFNOSUPPORT;
    return -1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    const uint8_t *a = src;
    if (af == AF_INET) {
        if (snprintf(dst, size, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]) >= (int)size)
            { errno = ENOSPC; return NULL; }
        return dst;
    }
    if (af == AF_INET6) {
        char *p = dst; size_t left = size;
        for (int i = 0; i < 16; i += 2) {
            int n = snprintf(p, left, i ? ":%x" : "%x", (a[i] << 8) | a[i + 1]);
            if (n < 0 || (size_t)n >= left) { errno = ENOSPC; return NULL; }
            p += n; left -= n;
        }
        return dst;
    }
    errno = EAFNOSUPPORT;
    return NULL;
}

int inet_aton(const char *cp, struct in_addr *inp)
{ return inet_pton(AF_INET, cp, &inp->s_addr) == 1; }
in_addr_t inet_addr(const char *cp)
{ struct in_addr a; return inet_aton(cp, &a) ? a.s_addr : INADDR_NONE; }
char *inet_ntoa(struct in_addr in)
{ static char buf[INET_ADDRSTRLEN]; return (char *)inet_ntop(AF_INET, &in, buf, sizeof buf); }

#include <sys/select.h>
#include <kernel.h>
/* select() with no descriptors is a portable sleep; anything else would be
 * waiting on a socket, and there are none. */
int select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
    (void)r; (void)w; (void)e;
    if (nfds == 0 && tv) {
        kthread_sleep(TICKS_FROM_US((uint64_t)tv->tv_sec * 1000000 + tv->tv_usec));
        return 0;
    }
    DOWN(-1);
}
