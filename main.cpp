#include "proxy_server.h"

int main() {
    try {
        shared_epoll epoll = std::make_shared<epoll_wrap>(epoll_wrap(200));
        proxy_server proxy(epoll, 8080, 200);


        shared_signal_fd sig_fd = std::make_shared<signal_fd>(SIGINT, signal_fd::SIMPLE);

        epoll_wrap::handler_t signal_handler =
                [sig_fd, &proxy](fd_state state, epoll_wrap &epoll_w) mutable {
                    if (state.is(fd_state::IN)) {
                        struct signalfd_siginfo sinf;
                        long size = sig_fd->read(&sinf, sizeof(struct signalfd_siginfo));
                        if (size != sizeof(struct signalfd_siginfo)) {
                            return;
                        }
                        if (sinf.ssi_signo == SIGINT) {
                            log("\nepoll", "stopped");
                            epoll_w.stop_wait();
                        }
                    }
                };

        epoll_registration signal_registration(epoll, *sig_fd, fd_state::IN, signal_handler);

        log("epoll", "started");
        epoll->start_wait();

    } catch (annotated_exception const &e) {
        log(e);
    }
}
