#include <pwd.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <argp.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stddef.h>
#include <sys/un.h>
#include <libgen.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <stdbool.h>

#ifndef APP_VERSION
#define APP_VERSION "unknown"
#endif

static char args_doc[] = "-- APP_TO_RUN [args]";
static char doc[] = "Run me like your fancy systemd";
static error_t parser(int key, char arg[], struct argp_state *state);

static void version_printer(FILE *restrict stream, struct argp_state *restrict state)
{
    fprintf(stream, "%s\n", APP_VERSION);
}

/* Put all options 'short' names or 'keys' values here */
enum
{
    /*
        255 + N to ensure no printable ASCII will be selected as 'short' name
        by accident
    */
    ARG_LISTEN_STREAM = 255 + 1,
    ARG_LISTEN_DATAGRAM,
    ARG_LISTEN_SEQ,
    ARG_SOCKET_PROTOCOL,
    ARG_BACKLOG,
    ARG_SOCKET_USER,
    ARG_SOCKET_GROUP,
    ARG_SOCKET_MODE,
    ARG_DIRECTORY_MODE,
    ARG_LOCK_UNIX_SOCKETS,
    ARG_MARK,
    ARG_KEEP_ALIVE,
    ARG_KEEP_ALIVE_TIME,
    ARG_KEEP_ALIVE_INTERVAL,
    ARG_KEEP_ALIVE_PROBES,
    ARG_PRIORITY,
    ARG_RECEIVE_BUFFER,
    ARG_SEND_BUFFER,
    ARG_IP_TOS,
    ARG_IP_TTL,
    ARG_REUSE_PORT,
    ARG_REUSE_ADDR,
    ARG_IP_DSCP,
};
struct tos_item
{
    const char *name;
    const int value;
};

static struct tos_item tos_items[] = {
    {"low-delay", 0},
    {"throughput", 1},
    {"reliability", 2},
    {"low-cost", 3},
    {0},
};

static struct argp_option args[] = {
    {"ListenStream", ARG_LISTEN_STREAM, "STREAM"},
    {"ListenDatagram", ARG_LISTEN_DATAGRAM, "DATAGRAM"},
    {"ListenSequentialPacket", ARG_LISTEN_SEQ, "SEQ"},
    {"SocketProtocol", ARG_SOCKET_PROTOCOL, "PROT"},
    {"Backlog", ARG_BACKLOG, "BACKLOG"},
    {"SocketUser", ARG_SOCKET_USER, "USER"},
    {"SocketGroup", ARG_SOCKET_GROUP, "GROUP"},
    {"SocketMode", ARG_SOCKET_MODE, "MODE"},
    {"DirectoryMode", ARG_DIRECTORY_MODE, "MODE"},
    {"LockUnixSockets", ARG_LOCK_UNIX_SOCKETS, NULL, 0,
     "Will create $path/~$socket lock file and pass FD to executed process."
     " If eg. $LISTEN_FDS=1, then lock socket will have assigned"
     " FD=3+$LISTEN_FDS, FD=3+$LISTEN_FDS + 1, ... and so on."
     " This is implementation detail you SHOULD NOT use in production,"
     " but might be quirky enough when you start investigating:"
     " `why my fd has been assigned so big number and not XYZ`"},
    {"Mark", ARG_MARK, "MARK"},
    {"KeepAlive", ARG_KEEP_ALIVE},
    {"KeepAliveTimeSec", ARG_KEEP_ALIVE_TIME, "SEC"},
    {"KeepAliveIntervalSec", ARG_KEEP_ALIVE_INTERVAL, "SEC"},
    {"KeepAliveProbes", ARG_KEEP_ALIVE_PROBES, "N"},
    {"Priority", ARG_PRIORITY, "PRIORITY"},
    {"ReceiveBuffer", ARG_RECEIVE_BUFFER, "BYTES"},
    {"SendBuffer", ARG_SEND_BUFFER, "BYTES"},
    {"IPTTL", ARG_IP_TTL, "TTL"},
    {"IPTOS", ARG_IP_TOS, "TOS", 0, "Deprecated. Use --IPDSCP."},
    {"IPDSCP", ARG_IP_DSCP, "DSCP"},
    {"ReusePort", ARG_REUSE_PORT},
    {"ReuseAddress", ARG_REUSE_ADDR},
    {0}, /* end */
};

static struct argp argp = {args, parser, args_doc, doc};

struct keep_alive
{
    /* SO_KEEPALIVE */
    int enable;
    /* TCP_KEEPIDLE */
    uint32_t time;
    /* TCP_KEEPINTVL */
    uint32_t interval;
    /* TCP_KEEPCNT */
    uint32_t probes;
};

/* marks what feature is set to distinguis deliberate 0 value for expected 0 */
enum listen_on_options
{
    LO_REUSE_PORT = 0 << 1,
    LO_REUSE_ADDR = 1 << 1,
    LO_RECV_BUFFER = 2 << 1,
    LO_SEND_BUFFER = 3 << 1,
    LO_PRIORITY = 4 << 1,
    LO_TOS = 5 << 1,
    LO_TTL = 6 << 1,
};

struct listen_on
{
    struct listen_on *next;

    const char *socket_listen; /* human radable name */
    struct sockaddr_storage addr;
    socklen_t addr_len;
    uint32_t mark;
    /* SO_KEEPALIVE */
    struct keep_alive keep_alive;
    /* SO_PRIORITY */
    uint32_t priority;
    /* SO_RCVBUF */
    uint32_t send_buffer;
    /* SO_SNDBUF */
    uint32_t recv_buffer;

    /* IP_TOS, deprecated */
    uint32_t tos;
    /* IP_TOS */
    uint32_t dscp;

    /* IP_TTL */
    uint32_t ttl;

    /* SO_REUSEPORT */
    bool reuse_port;
    /* SO_REUSEADDR */
    bool reuse_addr;

    int fd;
    uint32_t socket_protocol;
};

struct arguments
{
    const char *app_to_run;
    int copy_args_from;

    /* NOTE(m): Wrap this into it's own struct if support for multiple
    listening sockets will be desirable */
    struct listen_on listeners;

    /* options for (non abstract) unix socket */
    uid_t user;
    gid_t group;
    mode_t socket_mode;
    mode_t directory_mode;
    int lock_unix_socket;

    /* options for listening */
    int backlog;
};

static struct listen_on *new_listen_on(struct listen_on *base)
{
    struct listen_on *lo = base;
    while (lo->next)
    {
        lo = lo->next;
    }
    lo->next = malloc(sizeof(*lo->next));
    memset(lo->next, 0, sizeof(*lo->next));
    return lo->next;
}

static int listen_on_size(struct listen_on *base)
{
    struct listen_on *lo = base;
    int size = 0;

    while (lo)
    {
        ++size;
        lo = lo->next;
    }
    return size;
}

static void listen_on_free(struct listen_on *lo)
{
    struct listen_on *current = lo;
    struct listen_on *next = NULL;

    while (current)
    {
        next = current->next;
        free(current);
        current = next;
    }
}

static void arguments_free(struct arguments *args)
{
    /* use next, base is not malloced */
    listen_on_free(args->listeners.next);
}

static struct listen_on *obtain_listen_on(struct arguments *args)
{
    if (args->listeners.addr.ss_family == 0)
    {
        return &args->listeners;
    }
    return new_listen_on(&args->listeners);
}

static int open_or_mkdir(int fd, const char *name, mode_t mode)
{
    if (mkdirat(fd, name, mode) && errno != EEXIST)
    {
        return -1;
    }

    int dir = openat(fd, name, O_CLOEXEC | O_DIRECTORY | O_RDONLY);
    return dir;
}

int is_unix_socket_on_fs(struct listen_on const *lo)
{
    if (lo->addr.ss_family != AF_UNIX)
    {
        return 0;
    }
    const struct sockaddr_un *sun = (const struct sockaddr_un *)&lo->addr;
    return sun->sun_path[0] != '\0';
}

static int create_path(const char *path, const struct arguments *arguments)
{
    /* This is 'hack' as path should always be sun_path */
    char path_dup[sizeof(((struct sockaddr_un *)NULL)->sun_path)];
    // NOLINTNEXTLINE: insecure, srecure
    memcpy(path_dup, path, sizeof(path_dup));

    char *dirs = dirname(path_dup);
    char *tok_state = NULL;
    // Root '/' is expected to be created by someone else
    int dir_fd = open_or_mkdir(-1, "/", 0);
    if (dir_fd < 0)
    {
        perror("root open failed");
        return 1;
    }

    char *tok = strtok_r(dirs, "/", &tok_state);
    while (tok != NULL)
    {
        errno = 0;
        int new_fd = open_or_mkdir(dir_fd, tok, arguments->directory_mode);
        if (new_fd < 0)
        {
            perror("dirdasz");
            return 1;
        }
        close(dir_fd);
        dir_fd = new_fd;

        /* advance to next token */
        tok = strtok_r(NULL, "/", &tok_state);
    }
    close(dir_fd);
    return 0;
}

static int set_sol(int fd, int arg, uint32_t opt)
{
    return setsockopt(fd, SOL_SOCKET, arg, &opt, sizeof(opt));
}

static int set_tos(int fd, int tos);
static int set_dscp(int fd, int dscp);
static int set_ttl(int fd, int ttl);
static int set_keepalive(int fd, struct keep_alive ka);
static int set_options(const struct listen_on *lo);
static int lock_unix_socket(const struct sockaddr_un *unix_addr);

int main(int argc, char *argv[])
{
    /*
        FOR THE LOVE OF... ENSURE ALL DESCRIPTORS NOT MEANT TO BE PASSED DOWN
        ARE USING O_CLOEXEC
    */
    argp_program_version_hook = version_printer;
    struct arguments arguments = {0};
    arguments.socket_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH; /* 666 */
    arguments.directory_mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;        /* 755 */
    arguments.user = getuid();
    arguments.group = getgid();
    arguments.backlog = 128;

    if (argp_parse(&argp, argc, argv, 0, 0, &arguments))
    {
        argp_help(&argp, stderr, ARGP_HELP_STD_HELP, argv[0]);
        exit(1);
    }

    if (arguments.app_to_run == NULL || strlen(arguments.app_to_run) == 0)
    {
        argp_help(&argp, stderr, ARGP_HELP_STD_HELP, argv[0]);
        exit(1);
    }

    fprintf(stderr, "App to run: %s\n", arguments.app_to_run);
    fprintf(stderr, "Arguments: ");
    for (int i = arguments.copy_args_from; i < argc; ++i)
    {
        fprintf(stderr, "%s ", argv[i]);
    }
    fprintf(stderr, "\n");

    for (struct listen_on *lo = &arguments.listeners; lo != NULL;)
    {
        fprintf(stderr, "Listening: %s\n", lo->socket_listen);
        // check if this is unix socket, wchich may require locking
        if (lo->addr.ss_family == AF_UNIX)
        {
            // create parent directiries
            struct sockaddr_un *unix_addr = (struct sockaddr_un *)&lo->addr;
            if (create_path(unix_addr->sun_path, &arguments))
            {
                exit(1);
            }

            if (arguments.lock_unix_socket && lock_unix_socket(unix_addr))
            {
                exit(1);
            }
        }

        if (set_options(lo))
        {
            exit(1);
        }

        if (bind(lo->fd, (struct sockaddr *)&lo->addr, lo->addr_len))
        {
            perror("bind");
            exit(1);
        }

        /* listen is not working on: UDP*/
        if (lo->socket_protocol != IPPROTO_UDP)
        {
            if (listen(lo->fd, arguments.backlog))
            {
                perror("listen");
                exit(1);
            }
        }

        fprintf(stderr, "ACTIVE FD=%d\n", lo->fd);

        lo = lo->next;
    }

    /* mimic systemd */
    char tmp[16] = {0};
    snprintf(tmp, sizeof(tmp) - 1, "%d", getpid()); /* NOLINT */
    setenv("LISTEN_PID", tmp, 1);

    /* NOLINTNEXTLINE */
    snprintf(tmp, sizeof(tmp) - 1, "%d", listen_on_size(&arguments.listeners));
    setenv("LISTEN_FDS", tmp, 1);

    setenv("LISTEN_FDNAMES", "", 1);

    struct rlimit limits = {0};
    if (getrlimit(RLIMIT_NOFILE, &limits))
    {
        perror("getrlimit");
        exit(1);
    }

    /* cleanup mess */
    arguments_free(&arguments);

    /* app_to_run == argv[copy_args_from] */
    return execv(arguments.app_to_run, argv + arguments.copy_args_from);
}

static int parse_ul(const char *v, const int base, unsigned long *out)
{
    char *end = NULL;
    *out = strtoul(v, &end, base);
    if (errno != 0)
    {
        return errno;
    }

    /* check if whole string was parsed */
    if (end == NULL || strlen(end) > 0)
    {
        return EINVAL;
    }

    return 0;
}

static int parse_int10(const char *v, int *out)
{
    unsigned long parsed = 0;
    if (parse_ul(v, 10, &parsed))
    {
        return EINVAL;
    }
    *out = (int)parsed;
    return 0;
}

static int parse_uint32(const char *v, uint32_t *out)
{
    unsigned long parsed = 0;
    if (parse_ul(v, 10, &parsed))
    {
        return EINVAL;
    }

    if (parsed < 0)
    {
        return EINVAL;
    }

    if (parsed > 0xFFFFFFFF)
    {
        return EINVAL;
    }

    *out = (uint32_t)parsed;

    return 0;
}
#if 0
static int parse_bool(const char *v, int *out)
{
    size_t len = strlen(v);
    printf("%ld %s\n", len, v);
    if (len == 0)
    {
        return EINVAL;
    }
    // YES, this will pass yeti, and turban as true
    if (*v == '1' || tolower(*v) == 't' || tolower(*v) == 'y')
    {
        *out = 1;
        return 0;
    }
    *out = 0;
    return 0;
}
#endif

static int parse_user(const char *v, uid_t *user)
{
    unsigned long parsed = 0;

    fprintf(stderr, "Try parse user: %s\n", v);

    // this might be 'human readable' or number
    struct passwd *result = getpwnam(v);
    if (result != NULL)
    {
        *user = result->pw_uid;
        fprintf(stderr, "found user %d\n", *user);
        return 0;
    }

    if (parse_ul(v, 10, &parsed))
    {
        goto err;
    }

    *user = parsed;
    return 0;

err:
    fprintf(stderr, "not known user: %s\n", v);
    return EINVAL;
}

static int parse_group(const char *v, gid_t *group)
{
    unsigned long parsed = 0;

    if (parse_ul(v, 10, &parsed))
    {
        goto err;
    }

    return 0;
err:
    return EINVAL;
}

static int parse_mode(const char *v, mode_t *mode)
{
    const mode_t MAX_MODE = S_IRWXU | S_IRWXG | S_IRWXO;
    unsigned long parsed = 0;
    if (parse_ul(v, 8, &parsed))
    {
        goto err;
    }

    if (parsed > MAX_MODE)
    {
        goto err;
    }

    *mode = (mode_t)parsed;
    return 0;

err:
    fprintf(stderr, "value (%s) not valid mode\n", v);
    return EINVAL;
}

static int parse_us(const char *v, unsigned short *out)
{
    unsigned long parsed = 0;
    if (parse_ul(v, 10, &parsed))
    {
        return EINVAL;
    }

    if (parsed > 0xFFFF)
    {
        return EINVAL;
    }

    *out = (unsigned short)parsed;
    return 0;
}
static int parse_addr(const char *v, int type, struct listen_on *lo)
{
    unsigned short port = 0;

    if (parse_us(v, &port) == 0)
    {
        // Good, only port listen on any address
        struct sockaddr_in *in = (struct sockaddr_in *)&lo->addr;
        lo->addr_len = sizeof(struct sockaddr_in);

        in->sin_port = htons(port);
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = htonl(INADDR_ANY);

        goto ok;
    }

    size_t v_len = strlen(v);
    if (v_len < 2)
    {
        return EINVAL;
    }

    // try parsing as unix socket
    lo->addr_len = offsetof(struct sockaddr_un, sun_path) + v_len + 1;
    // lo->socket_domain = AF_UNIX;
    struct sockaddr_un *unix_addr = (struct sockaddr_un *)&lo->addr;
    unix_addr->sun_family = AF_UNIX;

    if (v[0] == '/')
    {
        strncpy(unix_addr->sun_path, v, sizeof(unix_addr->sun_path) - 1);
        goto ok;
    }
    else if (v[1] == '@')
    {
        lo->addr_len -= 1;
        strncpy(unix_addr->sun_path + 1, v + 1, sizeof(unix_addr->sun_path) - 2);
        goto ok;
    }

    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = type,
        .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV,
    };
    struct addrinfo *serverinfo = NULL;
    struct addrinfo *p = NULL;

    /* BUG(m): This will pearshape on IPv6 */
    char *p_semicolon = strchr(v, ':');
    if (p_semicolon == NULL)
    {
        fprintf(stderr, "missing port in endpoint\n");
        goto err;
    }

    char v_dup[INET6_ADDRSTRLEN] = {0};
    strncpy(v_dup, v, p_semicolon - v);

    if (getaddrinfo(v_dup, p_semicolon + 1, &hints, &serverinfo))
    {
        if (errno)
        {
            perror("getaddrinfo");
        }
        else
        {
            fprintf(stderr, "Unable to resolve address: %s\n", v);
        }
        goto err;
    }

    // Get first and call it a day:
    lo->addr_len = serverinfo->ai_addrlen;
    memcpy(&lo->addr, serverinfo->ai_addr, serverinfo->ai_addrlen);

    if (serverinfo->ai_family != lo->addr.ss_family)
    {
        fprintf(stderr, "Family missmatch\n");
        exit(1);
    }

    if (serverinfo->ai_socktype != type)
    {
        fprintf(stderr, "SOCKTYPE missmatch\n");
        exit(1);
    }

    lo->socket_protocol = serverinfo->ai_protocol;
    freeaddrinfo(serverinfo);

ok:
    lo->socket_listen = v;
    lo->fd = socket(lo->addr.ss_family, type, lo->socket_protocol);
    if (lo->fd < 0)
    {
        perror("socket");
        return errno;
    }
    return 0;

err:
    return EINVAL;
}
static error_t parser(int key, char arg[], struct argp_state *state)
{
    struct arguments *arguments = state->input;
    struct listen_on *lo = &arguments->listeners;

    switch (key)
    {
    case ARG_DIRECTORY_MODE:
        return parse_mode(arg, &arguments->directory_mode);
    case ARG_SOCKET_MODE:
        /* expecting octal number */
        return parse_mode(arg, &arguments->socket_mode);
    case ARG_SOCKET_USER:
        return parse_user(arg, &arguments->user);
    case ARG_SOCKET_GROUP:
        return parse_group(arg, &arguments->group);
    case ARG_BACKLOG:
        return parse_int10(arg, &arguments->backlog);
    case ARG_LISTEN_STREAM:
        lo = obtain_listen_on(arguments);
        return parse_addr(arg, SOCK_STREAM, lo);
    case ARG_LISTEN_DATAGRAM:
        lo = obtain_listen_on(arguments);
        return parse_addr(arg, SOCK_DGRAM, lo);
    case ARG_LISTEN_SEQ:
        lo = obtain_listen_on(arguments);
        return parse_addr(arg, SOCK_SEQPACKET, lo);
    case ARG_LOCK_UNIX_SOCKETS:
        arguments->lock_unix_socket = 1;
        break;
    case ARG_MARK:
        return parse_uint32(arg, &lo->mark);
    case ARG_KEEP_ALIVE:
        lo->keep_alive.enable = true;
        break;
    case ARG_KEEP_ALIVE_INTERVAL:
        return parse_uint32(arg, &lo->keep_alive.interval);
    case ARG_KEEP_ALIVE_PROBES:
        return parse_uint32(arg, &lo->keep_alive.probes);
    case ARG_KEEP_ALIVE_TIME:
        return parse_uint32(arg, &lo->keep_alive.time);
    case ARG_REUSE_ADDR:
        lo->reuse_addr = true;
        break;
    case ARG_REUSE_PORT:
        lo->reuse_port = true;
        break;
    case ARG_SEND_BUFFER:
        return parse_uint32(arg, &lo->send_buffer);
    case ARG_RECEIVE_BUFFER:
        return parse_uint32(arg, &lo->recv_buffer);
    case ARG_IP_TOS:
        // IPTOS_LOWDELAY and company
        fprintf(stderr, "WARNING: ToS has been deprecated. Use DSCP\n");
        return parse_uint32(arg, &lo->tos);
    case ARG_IP_TTL:
        return parse_uint32(arg, &lo->ttl);
    case ARG_PRIORITY:
        return parse_uint32(arg, &lo->priority);
    case ARG_IP_DSCP:
        return parse_uint32(arg, &lo->dscp);

    case ARGP_KEY_ARG:
        if (!state->quoted)
        {
            fprintf(stderr, "Application to run and it's arguments should be set after --\n");
            return ARGP_ERR_UNKNOWN;
        }
        if (state->arg_num == 0)
        {
            arguments->app_to_run = arg;
            arguments->copy_args_from = state->next - 1;
            state->next = state->argc;
            break;
        }
        fprintf(stderr, "SHOULD NET BE HERE: next=%d\n", state->next);
        break;
    case ARGP_KEY_END:
        if (state->arg_num < 1)
        {
            // argp_err_exit_status = EINVAL;
            argp_state_help(state, stdout, ARGP_HELP_STD_HELP | ARGP_HELP_EXIT_ERR);
            argp_error(state, "missing whay you want to run");
            return ARGP_ERR_UNKNOWN;
        }
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

/* be warned: this is deprecated, and wild thing might happen */
int set_tos(int fd, int tos)
{
    int masked = tos & IPTOS_TOS_MASK;
    if (masked != tos)
    {
        fprintf(stderr, "Invalid tos value: %d (%d)\n", tos, masked);
        return EINVAL;
    }
    return setsockopt(fd, IPPROTO_IP, IP_TOS, &masked, sizeof(masked));
}

int set_dscp(int fd, int val)
{
    if (val > 63)
    {
        return EINVAL;
    }
    /* shift: two first bits are ECN (Explicit Congestion Notification) */
    val = val << 2;
    int ret = setsockopt(fd, IPPROTO_IP, IP_TOS, &val, sizeof val);
    /* might not be IPv4, try IPv6 */
    if (ret)
    {
        return setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &val, sizeof val);
    }
    return 0;
}

int set_ttl(int fd, int ttl)
{
    int ret = setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
    if (ret)
    {
        return setsockopt(fd, IPPROTO_IP, IPV6_HOPLIMIT, &ttl, sizeof(ttl));
    }
    return 0;
}

static int set_tcpopt(int fd, int arg, int val)
{
    return setsockopt(fd, SOL_TCP, arg, &val, sizeof(val));
}

int set_keepalive(int fd, struct keep_alive keep_alive)
{
    if (!keep_alive.enable)
    {
        return 0;
    }

    if (set_sol(fd, SO_KEEPALIVE, keep_alive.enable))
    {
        perror("keepalive");
        exit(1);
    }

    if (keep_alive.time && set_tcpopt(fd, TCP_KEEPIDLE, keep_alive.time))
    {
        perror("keepidle");
        exit(1);
    }

    if (keep_alive.probes && set_tcpopt(fd, TCP_KEEPCNT, keep_alive.probes))
    {
        perror("keepcnt");
        exit(1);
    }

    if (keep_alive.interval && set_tcpopt(fd, TCP_KEEPINTVL, keep_alive.interval))
    {
        perror("keepintv");
        exit(1);
    }

    return 0;
}

static int set_options(const struct listen_on *lo)
{
    int fd = lo->fd;

    if (lo->mark && set_sol(fd, SO_MARK, lo->mark))
    {
        perror("SO_MARK");
        fprintf(stderr, "Unable to set mark %u to %s\n", lo->mark, lo->socket_listen);
        return 1;
    }

    if (lo->priority && set_sol(fd, SO_PRIORITY, lo->priority))
    {
        perror("priority");
        return 1;
    }

    if (lo->reuse_port && set_sol(fd, SO_REUSEPORT, lo->reuse_port))
    {
        perror("reuse port");
        return 1;
    }

    if (lo->reuse_addr && set_sol(fd, SO_REUSEADDR, lo->reuse_addr))
    {
        perror("reuse addr");
        return 1;
    }

    if (set_keepalive(fd, lo->keep_alive))
    {
        perror("keepalive");
        return 1;
    }

    if (lo->recv_buffer && set_sol(fd, SO_RCVBUF, lo->recv_buffer))
    {
        perror("rcv");
        return 1;
    }

    if (lo->send_buffer && set_sol(fd, SO_SNDBUF, lo->send_buffer))
    {
        perror("snd");
        return 1;
    }

    if (lo->ttl && set_ttl(fd, lo->ttl))
    {
        perror("ttl");
        return 1;
    }

    if (lo->tos && set_tos(fd, lo->tos))
    {
        perror("tos");
        return 1;
    }

    /* ToS is deprecated, so ensure dscp is set after it  in case where
       both values are present
    */
    if (lo->dscp && set_dscp(fd, lo->dscp))
    {
        perror("DSCP");
        return 1;
    }

    return 0;
}

static int lock_unix_socket(const struct sockaddr_un *unix_addr)
{
    if (unix_addr->sun_path[0] == '\0')
    {
        return 0;
    }

    // if name is not abstract check lock
    char path[PATH_MAX];

    /* copy is needed, dirname will destroy content of passed string */
    char sun_cpy1[sizeof(unix_addr->sun_path)];
    char sun_cpy2[sizeof(unix_addr->sun_path)];

    memcpy(sun_cpy1, unix_addr->sun_path, sizeof(sun_cpy1)); /* NOLINT */
    memcpy(sun_cpy2, unix_addr->sun_path, sizeof(sun_cpy1)); /* NOLINT */

    char *dir_name = dirname(sun_cpy1);
    char *file_name = basename(sun_cpy2);

    /* NOLINTNEXTLINE */
    snprintf(path, sizeof(path) - 1, "%s/~%s", dir_name, file_name);

    /*
        this is 'hackinsh' as now we have leaking descriptor to app,
        that is not aware of leaked descriptor, and listening sockets
        might be spread all around.
    */
    int flock_fd = open(path, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);

    /* there is now simple way for using flock:
        - passing 'flock' fd to child (after expected FDs)
        - faking flock like 'pid' file
    */
    if (flock_fd < 0)
    {
        perror("open");
        fprintf(stderr, "Unable to create lock file: %s\n", path);
        return 1;
    }

    if (flock(flock_fd, LOCK_EX | LOCK_NB))
    {
        perror("flock");
        fprintf(stderr, "Someone else is using socket: %s\n", unix_addr->sun_path);
        return 1;
    }

    int ret = unlink(unix_addr->sun_path);
    if (ret != 0 && errno != ENOENT)
    {
        perror("unlink");
        fprintf(stderr, "Unable to unlink socket file: %s\n", unix_addr->sun_path);
        return 1;
    }
    return 0;
}
