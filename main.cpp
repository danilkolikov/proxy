#include "proxy/proxy_server.h"

int main(int argc, char** args) {
    try {
        uint16_t port = 8080;
        if (argc > 1) {
            port = (uint16_t) std::stoi(args[1]);
        }

        epoll_wrap epoll(200);
        resolver<proxy_server::resolver_extra> ip_resolver;
        proxy_server proxy(epoll, ip_resolver, port, 200);

        std::string tag = "server on port " + std::to_string(port);
        signal_fd sig_fd({SIGINT, SIGPIPE}, {signal_fd::SIMPLE});

        epoll_registration signal_registration(epoll, std::move(sig_fd), fd_state::IN);
        signal_registration.update([&signal_registration, &epoll, tag](fd_state state) mutable {
            if (state.is(fd_state::IN)) {
                struct signalfd_siginfo sinf;
                long size = signal_registration.get_fd().read(&sinf, sizeof(struct signalfd_siginfo));
                if (size != sizeof(struct signalfd_siginfo)) {
                    return;
                }
                if (sinf.ssi_signo == SIGINT) {
                    log("\n" + tag, "stopped");
                    epoll.stop_wait();
                }
            }
        });

        log(tag, "started");
        epoll.start_wait();

    } catch (annotated_exception const &e) {
        log(e);
    }
}
