#ifndef WRAPS_H_
#define WRAPS_H_

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/ioctl.h>

#include <signal.h>
#include <netinet/in.h>
#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <cstdlib>
#include <fcntl.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>

#include "util.h"

// Wrap for unix file descriptor
struct file_descriptor {
    explicit file_descriptor(int fd);

    file_descriptor(file_descriptor &&other);
    file_descriptor &operator=(file_descriptor &&other);

    virtual ~file_descriptor();

    // Get number of file descriptor
    int get() const;

    // Can we read from it
    int can_read() const;
    long read(void *message, size_t message_size) const;
    long write(void const *message, size_t message_size) const;

    friend void swap(file_descriptor &first, file_descriptor &second);
    friend std::string to_string(file_descriptor const &fd);
protected:
    file_descriptor();
    int fd;
};

// Wrap for timer_fd
struct timer_fd : file_descriptor {
    enum clock_mode {
        REALTIME, MONOTONIC
    };
    enum fd_mode {
        NONBLOCK, CLOEXEC, SIMPLE
    };

    timer_fd();
    timer_fd(clock_mode cmode, fd_mode mode);
    timer_fd(clock_mode cmode, std::initializer_list<fd_mode> mode);

    // Set interval for ticking
    void set_interval(long interval_sec, long start_after_sec) const;

private:
    int value_of(std::initializer_list<fd_mode> mode);
};

// Wrap for event_fd
struct event_fd : file_descriptor {
    enum fd_mode {
        NONBLOCK, CLOEXEC, SEMAPHORE, SIMPLE
    };

    event_fd();
    event_fd(unsigned int initial, fd_mode mode);
    event_fd(unsigned int initial, std::initializer_list<fd_mode> mode);

private:
    int value_of(std::initializer_list<fd_mode> mode);
};

// Wrap for signal_fd
struct signal_fd : file_descriptor {
    enum fd_mode {
        NONBLOCK, CLOEXEC, SIMPLE
    };

    signal_fd();
    signal_fd(int hande, fd_mode mode);
    signal_fd(std::initializer_list<int> handle, std::initializer_list<fd_mode> mode);

private:
    int value_of(std::initializer_list<fd_mode> mode);
};

// IPv4 endpoint
struct endpoint {
    uint32_t ip;
    uint16_t port;

};
void swap(endpoint &first, endpoint &second);
std::string to_string(endpoint const &ep);

// Wrap for socket
struct socket_wrap : file_descriptor {
    enum socket_mode {
        NONBLOCK, CLOEXEC, SIMPLE
    };

    socket_wrap(socket_mode mode);
    socket_wrap(std::initializer_list<socket_mode> mode);
    socket_wrap(socket_wrap &&other);

    // Accept other socket
    socket_wrap accept(socket_mode mode) const;
    socket_wrap accept(std::initializer_list<socket_mode> mode) const;

    // Bing to port
    void bind(uint16_t port) const;

    // Connect to endpoint
    void connect(endpoint address) const;

    // Listen to incoming connections
    void listen(int queue_size) const;

    // Method that calls getsockopt
    void get_option(int name, void *res, socklen_t *res_len) const;

    friend std::string to_string(socket_wrap &wrap);
protected:
    socket_wrap();
    explicit socket_wrap(int fd);

private:
    int value_of(std::initializer_list<socket_mode> modes) const;
};

// State of file descriptor in epoll
struct fd_state {
    enum state {
        IN, OUT, WAIT, ERROR, HUP, RDHUP
    };

    fd_state();
    fd_state(uint32_t st);
    fd_state(state st);
    fd_state(std::initializer_list<state> st);
    fd_state(fd_state const &other);
    fd_state(fd_state &&other);
    fd_state &operator=(fd_state other);

    bool operator!=(fd_state other) const;

    bool is(fd_state st) const;
    uint32_t get() const;

    friend fd_state operator^(fd_state first, fd_state second);
    friend fd_state operator|(fd_state first, fd_state second);

    friend void swap(fd_state &first, fd_state &second);
    friend bool operator==(fd_state const &first, fd_state const &second);
private:
    static uint32_t value_of(std::initializer_list<state> st);

    uint32_t fd_st;
};

// Wrap for epoll. After state of file_descriptor becomes equal to state it was registered to,
// handler is called with current fd_state
struct epoll_wrap : file_descriptor {
    using handler_t = std::function<void(fd_state)>;

    epoll_wrap(int max_queue_size);
    epoll_wrap(epoll_wrap &&other);

    // Register file descriptor in epoll
    void register_fd(const file_descriptor &fd, fd_state events);
    void register_fd(const file_descriptor &fd, fd_state events, handler_t handler);

    // Unregister
    void unregister_fd(const file_descriptor &fd);

    // Update state (and handler) of file descriptor
    void update_fd(const file_descriptor &fd, fd_state events);
    void update_fd_handler(const file_descriptor &fd, handler_t handler);

    // Start epoll
    void start_wait();

    // Say epoll that it should stop
    void stop_wait();

    friend void swap(epoll_wrap &first, epoll_wrap &second);
private:
    using handlers_t = std::map<int, handler_t>;

    epoll_event create_event(int fd, fd_state const &events);

    int queue_size;
    std::unique_ptr<epoll_event[]> events;
    handlers_t handlers;
    volatile bool started, stopped;
};

using shared_epoll = std::shared_ptr<epoll_wrap>;

// RAII structure for registration in epoll. In constructor registers, in destructor unregisters
struct epoll_registration {
    epoll_registration();
    epoll_registration(epoll_wrap &epoll, file_descriptor &&fd, fd_state state);
    epoll_registration(epoll_wrap &epoll, file_descriptor &&fd, fd_state state, epoll_wrap::handler_t handler);
    epoll_registration(epoll_registration &&other);
    epoll_registration &operator=(epoll_registration &&other);

    ~epoll_registration();

    file_descriptor &get_fd();
    file_descriptor const &get_fd() const;

    fd_state get_state() const;
    void update(fd_state state);
    void update(epoll_wrap::handler_t handler);
    void update(fd_state state, epoll_wrap::handler_t handler);

    friend void swap(epoll_registration &frist, epoll_registration &other);
    friend std::string to_string(epoll_registration &er);
private:
    epoll_wrap *epoll; // It's here because of epoll_registration should be move-constructable
    file_descriptor fd;
    fd_state events;
};

#endif /* WRAPS_H_ */
