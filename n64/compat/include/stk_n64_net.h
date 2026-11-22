/* Network API surface for the N64 (see n64/compat/src/net_stub.c).
 *
 * The N64 has no network hardware. Rather than #ifdef STK's network code
 * out (it is threaded through ~100 files: rewind, packet serialisation and
 * NetworkConfig are used offline too), the platform provides the POSIX
 * socket API and every call fails cleanly with ENETDOWN -- the same thing a
 * PC without a network sees, which STK already handles. Real networking
 * later (e.g. over a flashcart's USB link) means replacing net_stub.c, not
 * touching the game.
 */
#ifndef STK_N64_NET_H
#define STK_N64_NET_H
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/time.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;
typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_INET   2
#define AF_INET6  10
#define AF_MAX    11
#define PF_UNSPEC AF_UNSPEC
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3

#define SOL_SOCKET   1
#define SO_DEBUG     1
#define SO_REUSEADDR 2
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_DONTROUTE 5
#define SO_BROADCAST 6
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define SO_KEEPALIVE 9
#define SO_OOBINLINE 10
#define SO_LINGER    13
#define SO_REUSEPORT 15
#define SO_RCVLOWAT  18
#define SO_SNDLOWAT  19
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21

#define MSG_OOB       0x01
#define MSG_PEEK      0x02
#define MSG_DONTROUTE 0x04
#define MSG_TRUNC     0x20
#define MSG_DONTWAIT  0x40
#define MSG_WAITALL   0x100
#define MSG_NOSIGNAL  0x4000

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define SOMAXCONN 128

#define IPPROTO_IP    0
#define IPPROTO_ICMP  1
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17
#define IPPROTO_IPV6  41
#define IPPROTO_RAW   255

#define IP_TOS              1
#define IP_TTL              2
#define IP_MULTICAST_IF     32
#define IP_MULTICAST_TTL    33
#define IP_MULTICAST_LOOP   34
#define IP_ADD_MEMBERSHIP   35
#define IP_DROP_MEMBERSHIP  36
#define IPV6_V6ONLY         26
#define IPV6_JOIN_GROUP     20
#define IPV6_LEAVE_GROUP    21
#define TCP_NODELAY         1

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE      ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)
#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

struct sockaddr { sa_family_t sa_family; char sa_data[14]; };
struct sockaddr_storage { sa_family_t ss_family; char __ss_pad[126]
    __attribute__((aligned(8))); };
struct in_addr { in_addr_t s_addr; };
struct sockaddr_in {
    sa_family_t sin_family; in_port_t sin_port; struct in_addr sin_addr;
    unsigned char sin_zero[8];
};
struct in6_addr {
    union { uint8_t __u6_addr8[16]; uint16_t __u6_addr16[8];
            uint32_t __u6_addr32[4]; } __in6_u;
};
#define s6_addr   __in6_u.__u6_addr8
#define s6_addr16 __in6_u.__u6_addr16
#define s6_addr32 __in6_u.__u6_addr32
extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;
#define IN6ADDR_ANY_INIT { { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } } }
#define IN6ADDR_LOOPBACK_INIT { { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 } } }
struct sockaddr_in6 {
    sa_family_t sin6_family; in_port_t sin6_port; uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr; uint32_t sin6_scope_id;
};
struct sockaddr_un { sa_family_t sun_family; char sun_path[108]; };
struct ip_mreq { struct in_addr imr_multiaddr; struct in_addr imr_interface; };
struct ipv6_mreq { struct in6_addr ipv6mr_multiaddr; unsigned ipv6mr_interface; };
struct linger { int l_onoff; int l_linger; };
struct iovec;
struct msghdr {
    void *msg_name; socklen_t msg_namelen; struct iovec *msg_iov;
    size_t msg_iovlen; void *msg_control; size_t msg_controllen; int msg_flags;
};

#define IN6_IS_ADDR_V4MAPPED(a) \
    ((a)->s6_addr32[0] == 0 && (a)->s6_addr32[1] == 0 && \
     (a)->s6_addr32[2] == htonl(0x0000ffff))
#define IN6_IS_ADDR_UNSPECIFIED(a) \
    ((a)->s6_addr32[0] == 0 && (a)->s6_addr32[1] == 0 && \
     (a)->s6_addr32[2] == 0 && (a)->s6_addr32[3] == 0)
#define IN6_IS_ADDR_LOOPBACK(a) \
    ((a)->s6_addr32[0] == 0 && (a)->s6_addr32[1] == 0 && \
     (a)->s6_addr32[2] == 0 && (a)->s6_addr32[3] == htonl(1))

/* The N64 is big-endian: network byte order is host byte order. */
static inline uint32_t htonl(uint32_t x) { return x; }
static inline uint16_t htons(uint16_t x) { return x; }
static inline uint32_t ntohl(uint32_t x) { return x; }
static inline uint16_t ntohs(uint16_t x) { return x; }

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *to, socklen_t tolen);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *from, socklen_t *fromlen);
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags);
ssize_t recvmsg(int fd, struct msghdr *msg, int flags);
int setsockopt(int fd, int level, int name, const void *val, socklen_t len);
int getsockopt(int fd, int level, int name, void *val, socklen_t *len);
int getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int shutdown(int fd, int how);

in_addr_t inet_addr(const char *cp);
int inet_aton(const char *cp, struct in_addr *inp);
char *inet_ntoa(struct in_addr in);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int inet_pton(int af, const char *src, void *dst);

/* netdb */
struct addrinfo {
    int ai_flags; int ai_family; int ai_socktype; int ai_protocol;
    socklen_t ai_addrlen; struct sockaddr *ai_addr; char *ai_canonname;
    struct addrinfo *ai_next;
};
struct hostent {
    char *h_name; char **h_aliases; int h_addrtype; int h_length;
    char **h_addr_list;
};
#define h_addr h_addr_list[0]
#define AI_PASSIVE     0x01
#define AI_CANONNAME   0x02
#define AI_NUMERICHOST 0x04
#define AI_V4MAPPED    0x08
#define AI_ALL         0x10
#define AI_ADDRCONFIG  0x20
#define AI_NUMERICSERV 0x400
#define NI_MAXHOST 1025
#define NI_MAXSERV 32
#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
#define NI_NOFQDN      4
#define NI_NAMEREQD    8
#define NI_DGRAM       16
#define EAI_AGAIN    -3
#define EAI_BADFLAGS -1
#define EAI_FAIL     -4
#define EAI_FAMILY   -6
#define EAI_MEMORY   -10
#define EAI_NONAME   -2
#define EAI_SERVICE  -8
#define EAI_SOCKTYPE -7
#define EAI_SYSTEM   -11
int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int errcode);
int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host,
                socklen_t hostlen, char *serv, socklen_t servlen, int flags);
struct hostent *gethostbyname(const char *name);
struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type);
int gethostname(char *name, size_t len);
struct servent { char *s_name; char **s_aliases; int s_port; char *s_proto; };
struct protoent { char *p_name; char **p_aliases; int p_proto; };
struct servent *getservbyname(const char *name, const char *proto);
struct servent *getservbyport(int port, const char *proto);
struct protoent *getprotobynumber(int proto);
struct protoent *getprotobyname(const char *name);

/* poll */
struct pollfd { int fd; short events; short revents; };
typedef unsigned int nfds_t;
#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020
int poll(struct pollfd *fds, nfds_t nfds, int timeout);

/* interfaces */
struct ifaddrs {
    struct ifaddrs *ifa_next; char *ifa_name; unsigned int ifa_flags;
    struct sockaddr *ifa_addr; struct sockaddr *ifa_netmask;
    struct sockaddr *ifa_broadaddr; void *ifa_data;
};
#define ifa_dstaddr ifa_broadaddr
int getifaddrs(struct ifaddrs **ifap);
void freeifaddrs(struct ifaddrs *ifa);
unsigned int if_nametoindex(const char *ifname);
#define IFF_UP        0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK  0x8
#define IFF_RUNNING   0x40
#define IFF_MULTICAST 0x1000
#define IF_NAMESIZE   16

#ifdef __cplusplus
}
#endif
#endif
