#include "proxy_server.h"

int main() {
    try {
        epoll_wrap epoll(200);
        resolver<proxy_server::resolver_extra> ip_resolver;
        proxy_server proxy(epoll, ip_resolver, 8080, 200);


        signal_fd sig_fd(SIGINT, signal_fd::SIMPLE);

        epoll_registration signal_registration(epoll, std::move(sig_fd), fd_state::IN);
        signal_registration.update([&signal_registration, &epoll](fd_state state) mutable {
            if (state.is(fd_state::IN)) {
                struct signalfd_siginfo sinf;
                long size = signal_registration.get_fd().read(&sinf, sizeof(struct signalfd_siginfo));
                if (size != sizeof(struct signalfd_siginfo)) {
                    return;
                }
                if (sinf.ssi_signo == SIGINT) {
                    log("\nepoll", "stopped");
                    epoll.stop_wait();
                }
            }
        });

        log("epoll", "started");
        epoll.start_wait();

    } catch (annotated_exception const &e) {
        log(e);
    }
}
