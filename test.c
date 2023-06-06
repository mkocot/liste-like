#include <sys/resource.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <systemd/sd-daemon.h>

int main() {
    struct rlimit rlimit;
    if (getrlimit(RLIMIT_NOFILE, &rlimit)) {
        return 1;
    }

    for (int i = 0; i < rlimit.rlim_cur; ++i) {
        errno = 0;
        if (fcntl(i, F_GETFD) != -1 && errno != EBADF) {
            printf("Active fd=%d\n", i);
        }
    }
    int sockets_from_systemd = sd_listen_fds(0);
    printf("sockets_from_systemd=%d\n", sockets_from_systemd);
    // try accepting and sending response
    int ac = accept(3, NULL, NULL);
    char msg[] = "HelloWorld";
    send(ac, msg, sizeof(msg), 0);
    return 0;
}
