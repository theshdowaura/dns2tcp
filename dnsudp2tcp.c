#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "libev/ev.h"
#undef _GNU_SOURCE

#define DNS2TCP_VER "dns2tcp v1.1.0"

#define IP4STRLEN INET_ADDRSTRLEN /* ipv4addr max strlen */
#define IP6STRLEN INET6_ADDRSTRLEN /* ipv6addr max strlen */
#define PORTSTRLEN 6 /* "65535", include the null character */
#define UDPDGRAM_MAXSIZ 1472 /* mtu:1500 - iphdr:20 - udphdr:8 */

typedef uint16_t portno_t; /* 16bit */
typedef struct sockaddr_in  skaddr4_t;
typedef struct sockaddr_in6 skaddr6_t;

#define IF_VERBOSE if (g_verbose)

#define LOGINF(fmt, ...)                                                    \
    do {                                                                    \
        struct tm *tm = localtime(&(time_t){time(NULL)});                   \
        printf("\e[1;32m%04d-%02d-%02d %02d:%02d:%02d INF:\e[0m " fmt "\n", \
                tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,            \
                tm->tm_hour,        tm->tm_min,     tm->tm_sec,             \
                ##__VA_ARGS__);                                             \
    } while (0)

#define LOGERR(fmt, ...)                                                    \
    do {                                                                    \
        struct tm *tm = localtime(&(time_t){time(NULL)});                   \
        printf("\e[1;35m%04d-%02d-%02d %02d:%02d:%02d ERR:\e[0m " fmt "\n", \
                tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,            \
                tm->tm_hour,        tm->tm_min,     tm->tm_sec,             \
                ##__VA_ARGS__);                                             \
    } while (0)

typedef struct {
    evio_t    watcher;
    void     *buffer;
    uint16_t  nrecv;
    skaddr6_t srcaddr;
} tcpwatcher_t;

enum {
    OPT_IPV6_V6ONLY = 1 << 0,
    OPT_REUSE_PORT  = 1 << 1,
    OPT_QUICK_ACK   = 1 << 2,
    OPT_FAST_OPEN   = 1 << 3,
};

static bool       g_verbose                 = false;
static uint8_t    g_options                 = 0;
static uint8_t    g_syn_maxcnt              = 0;
static evloop_t  *g_event_loop              = NULL;
static evio_t     g_udp_watcher             = {0};
static char       g_listen_ipstr[IP6STRLEN] = {0};
static portno_t   g_listen_portno           = 0;
static skaddr6_t  g_listen_skaddr           = {0};
static char       g_remote_ipstr[IP6STRLEN] = {0};
static portno_t   g_remote_portno           = 0;
static skaddr6_t  g_remote_skaddr           = {0};

static void udp_recv_cb(evloop_t *evloop, evio_t *watcher, int events);
static void tcp_send_cb(evloop_t *evloop, evio_t *watcher, int events);
static void tcp_recv_cb(evloop_t *evloop, evio_t *watcher, int events);

static int get_ipstr_family(const char *ipstr) {
    if (!ipstr) return -1; /* invalid */
    uint8_t ipaddr[16]; /* 16-bytes */
    if (inet_pton(AF_INET, ipstr, &ipaddr) == 1) {
        return AF_INET;
    } else if (inet_pton(AF_INET6, ipstr, &ipaddr) == 1) {
        return AF_INET6;
    } else {
        return -1; /* invalid */
    }
}

static void build_sock_addr4(skaddr4_t *addr, const char *ipstr, portno_t portno) {
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, ipstr, &addr->sin_addr);
    addr->sin_port = htons(portno);
}
static void build_sock_addr6(skaddr6_t *addr, const char *ipstr, portno_t portno) {
    addr->sin6_family = AF_INET6;
    inet_pton(AF_INET6, ipstr, &addr->sin6_addr);
    addr->sin6_port = htons(portno);
}

static void parse_sock_addr4(const skaddr4_t *addr, char *ipstr, portno_t *portno) {
    inet_ntop(AF_INET, &addr->sin_addr, ipstr, IP4STRLEN);
    *portno = ntohs(addr->sin_port);
}
static void parse_sock_addr6(const skaddr6_t *addr, char *ipstr, portno_t *portno) {
    inet_ntop(AF_INET6, &addr->sin6_addr, ipstr, IP6STRLEN);
    *portno = ntohs(addr->sin6_port);
}

static void print_command_help(void) {
    printf("usage: dns2tcp <-L listen> <-R remote> [-s syncnt] [-6rafvVh]\n"
           " -L <ip#port>            udp listen address, this is required\n"
           " -R <ip#port>            tcp remote address, this is required\n"
           " -s <syncnt>             set TCP_SYNCNT(max) for remote socket\n"
           " -6                      enable IPV6_V6ONLY for listen socket\n"
           " -r                      enable SO_REUSEPORT for listen socket\n"
           " -a                      enable TCP_QUICKACK for remote socket\n"
           " -f                      enable TCP_FASTOPEN for remote socket\n"
           " -v                      print verbose log, default: <disabled>\n"
           " -V                      print version number of dns2tcp and exit\n"
           " -h                      print help information of dns2tcp and exit\n"
           "bug report: https://github.com/zfl9/dns2tcp. email: zfl9.com@gmail.com\n"
    );
}

static void parse_address_opt(char *ip_port_str, bool is_listen_addr) {
    const char *opt_name = is_listen_addr ? "listen" : "remote";

    char *portstr = strchr(ip_port_str, '#');
    if (!portstr) {
        printf("[parse_address_opt] %s port is not specified\n", opt_name);
        goto PRINT_HELP_AND_EXIT;
    }
    if (portstr == ip_port_str) {
        printf("[parse_address_opt] %s addr is not specified\n", opt_name);
        goto PRINT_HELP_AND_EXIT;
    }

    *portstr = 0; ++portstr;
    if (strlen(portstr) + 1 > PORTSTRLEN) {
        printf("[parse_address_opt] %s port is invalid: %s\n", opt_name, portstr);
        goto PRINT_HELP_AND_EXIT;
    }
    portno_t portno = strtoul(portstr, NULL, 10);
    if (portno == 0) {
        printf("[parse_address_opt] %s port is invalid: %s\n", opt_name, portstr);
        goto PRINT_HELP_AND_EXIT;
    }

    char *ipstr = ip_port_str;
    if (strlen(ipstr) + 1 > IP6STRLEN) {
        printf("[parse_address_opt] %s addr is invalid: %s\n", opt_name, ipstr);
        goto PRINT_HELP_AND_EXIT;
    }
    int ipfamily = get_ipstr_family(ipstr);
    if (ipfamily == -1) {
        printf("[parse_address_opt] %s addr is invalid: %s\n", opt_name, ipstr);
        goto PRINT_HELP_AND_EXIT;
    }

    void *skaddr_ptr = is_listen_addr ? &g_listen_skaddr : &g_remote_skaddr;
    if (ipfamily == AF_INET) {
        build_sock_addr4(skaddr_ptr, ipstr, portno);
    } else {
        build_sock_addr6(skaddr_ptr, ipstr, portno);
    }

    if (is_listen_addr) {
        strcpy(g_listen_ipstr, ipstr);
        g_listen_portno = portno;
    } else {
        strcpy(g_remote_ipstr, ipstr);
        g_remote_portno = portno;
    }
    return;

PRINT_HELP_AND_EXIT:
    print_command_help();
    exit(1);
}

static void parse_command_args(int argc, char *argv[]) {
    char opt_listen_addr[IP6STRLEN + PORTSTRLEN] = {0};
    char opt_remote_addr[IP6STRLEN + PORTSTRLEN] = {0};

    opterr = 0;
    int shortopt = -1;
    const char *optstr = "L:R:s:6rafvVh";
    while ((shortopt = getopt(argc, argv, optstr)) != -1) {
        switch (shortopt) {
            case 'L':
                if (strlen(optarg) + 1 > IP4STRLEN + PORTSTRLEN) {
                    printf("[parse_command_args] invalid listen addr: %s\n", optarg);
                    goto PRINT_HELP_AND_EXIT;
                }
                strcpy(opt_listen_addr, optarg);
                break;
            case 'R':
                if (strlen(optarg) + 1 > IP6STRLEN + PORTSTRLEN) {
                    printf("[parse_command_args] invalid remote addr: %s\n", optarg);
                    goto PRINT_HELP_AND_EXIT;
                }
                strcpy(opt_remote_addr, optarg);
                break;
            case 's':
                g_syn_maxcnt = strtoul(optarg, NULL, 10);
                if (g_syn_maxcnt == 0) {
                    printf("[parse_command_args] invalid tcp syn cnt: %s\n", optarg);
                    goto PRINT_HELP_AND_EXIT;
                }
                break;
            case '6':
                g_options |= OPT_IPV6_V6ONLY;
                break;
            case 'r':
                g_options |= OPT_REUSE_PORT;
                break;
            case 'a':
                g_options |= OPT_QUICK_ACK;
                break;
            case 'f':
                g_options |= OPT_FAST_OPEN;
                break;
            case 'v':
                g_verbose = true;
                break;
            case 'V':
                printf(DNS2TCP_VER"\n");
                exit(0);
            case 'h':
                print_command_help();
                exit(0);
            case '?':
                if (!strchr(optstr, optopt)) {
                    printf("[parse_command_args] unknown option '-%c'\n", optopt);
                } else {
                    printf("[parse_command_args] missing optval '-%c'\n", optopt);
                }
                goto PRINT_HELP_AND_EXIT;
        }
    }

    if (strlen(opt_listen_addr) == 0) {
        printf("[parse_command_args] missing option: '-L'\n");
        goto PRINT_HELP_AND_EXIT;
    }
    if (strlen(opt_remote_addr) == 0) {
        printf("[parse_command_args] missing option: '-R'\n");
        goto PRINT_HELP_AND_EXIT;
    }

    parse_address_opt(opt_listen_addr, true);
    parse_address_opt(opt_remote_addr, false);
    return;

PRINT_HELP_AND_EXIT:
    print_command_help();
    exit(1);
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 256);
    parse_command_args(argc, argv);

    LOGINF("[main] udp listen addr: %s#%hu", g_listen_ipstr, g_listen_portno);
    LOGINF("[main] tcp remote addr: %s#%hu", g_remote_ipstr, g_remote_portno);
    if (g_syn_maxcnt) LOGINF("[main] enable TCP_SYNCNT:%hhu sockopt", g_syn_maxcnt);
    if (g_options & OPT_IPV6_V6ONLY) LOGINF("[main] enable IPV6_V6ONLY sockopt");
    if (g_options & OPT_REUSE_PORT) LOGINF("[main] enable SO_REUSEPORT sockopt");
    if (g_options & OPT_QUICK_ACK) LOGINF("[main] enable TCP_QUICKACK sockopt");
    if (g_options & OPT_FAST_OPEN) LOGINF("[main] enable TCP_FASTOPEN sockopt");
    IF_VERBOSE LOGINF("[main] verbose mode, affect performance");

    g_event_loop = ev_default_loop(0);

    int udp_sfd = socket(g_listen_skaddr.sin6_family, SOCK_DGRAM, 0);
    if (udp_sfd < 0) {
        LOGERR("[main] create udp socket failed: (%d) %s", errno, strerror(errno));
        return errno;
    }

    // TODO

    return 0;
}
