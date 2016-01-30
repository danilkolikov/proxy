#include "wraps.h"

file_descriptor::file_descriptor() :
        fd(0) {
}

file_descriptor::file_descriptor(int fd) :
        fd(fd) {
    if (fd == -1) {
        int err = errno;
        throw annotated_exception("fd", err);
    }
}

file_descriptor::file_descriptor(file_descriptor &&other) : fd() {
    swap(*this, other);
}

file_descriptor &file_descriptor::operator=(file_descriptor &&other) {
    swap(*this, other);
    return *this;
}

file_descriptor::~file_descriptor() {
    if (fd != 0) {
        close(fd);
    }
}

int file_descriptor::get() const {
    return fd;
}

int file_descriptor::can_read() const {
    int bytes = 0;
    if (ioctl(fd, FIONREAD, &bytes) == -1) {
        int err = errno;
        throw annotated_exception("can_read", err);
    }
    return bytes;
}

long file_descriptor::read(void *message, size_t message_size) const {
    long read = ::read(fd, message, message_size);
    if (read == -1) {
        int err = errno;
        throw annotated_exception("read", err);
    }
    return read;
}

long file_descriptor::write(void const *message, size_t message_size) const {
    long written = ::write(fd, message, message_size);
    if (written == -1) {
        int err = errno;
        throw annotated_exception("write", err);
    }
    return written;
}

void swap(file_descriptor &first, file_descriptor &second) {
    std::swap(first.fd, second.fd);
}

std::string to_string(file_descriptor const& fd) {
    return "file descriptor " + std::to_string(fd.get());
}
timer_fd::timer_fd() : file_descriptor() {

}

timer_fd::timer_fd(clock_mode cmode, fd_mode mode) : timer_fd(cmode, {mode}) {
}

timer_fd::timer_fd(clock_mode cmode, std::initializer_list<fd_mode> mode) {
    int clock_mode = (cmode == MONOTONIC) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    if ((fd = timerfd_create(clock_mode, value_of(mode))) == -1) {
        int err = errno;
        throw annotated_exception("timerfd", err);
    }
}

void timer_fd::set_interval(long interval_sec, long start_after_sec) const {
    itimerspec spec;
    memset(&spec, 0, sizeof spec);
    spec.it_value.tv_sec = start_after_sec;
    spec.it_interval.tv_sec = interval_sec;
    if (timerfd_settime(fd, 0, &spec, 0) == -1) {
        int err = errno;
        throw annotated_exception("timerfd", err);
    }
}

int timer_fd::value_of(std::initializer_list<fd_mode> mode) {
    int res = 0;
    for (auto it = mode.begin(); it != mode.end(); it++) {
        switch (*it) {
            case NONBLOCK:
                res |= TFD_NONBLOCK;
                break;
            case CLOEXEC:
                res |= TFD_CLOEXEC;
                break;
            case SIMPLE:
                res |= 0;
                break;
        }
    }
    return res;
}

event_fd::event_fd() : file_descriptor() { }

event_fd::event_fd(unsigned int initial, fd_mode mode) : event_fd(initial, {mode}) { }

event_fd::event_fd(unsigned int initial, std::initializer_list<fd_mode> mode) {
    if ((fd = eventfd(initial, value_of(mode))) == -1) {
        int err = errno;
        throw annotated_exception("eventfd", err);
    }
}

int event_fd::value_of(std::initializer_list<fd_mode> mode) {
    int res = 0;
    for (auto it = mode.begin(); it != mode.end(); it++) {
        switch (*it) {
            case NONBLOCK:
                res |= EFD_NONBLOCK;
                break;
            case CLOEXEC:
                res |= EFD_CLOEXEC;
                break;
            case SEMAPHORE:
                res |= EFD_SEMAPHORE;
                break;
            case SIMPLE:
                res |= 0;
                break;
        }
    }
    return res;
}

signal_fd::signal_fd() : file_descriptor() { }

signal_fd::signal_fd(int handle, fd_mode mode) : signal_fd({handle}, {mode}) {
}

signal_fd::signal_fd(std::initializer_list<int> handle, std::initializer_list<fd_mode> mode) {
    sigset_t mask;
    sigemptyset(&mask);
    for (auto it = handle.begin(); it != handle.end(); it++) {
        sigaddset(&mask, *it);
    }
    sigprocmask(SIG_BLOCK, &mask, NULL);

    if ((fd = signalfd(-1, &mask, value_of(mode))) == -1) {
        int err = errno;
        throw annotated_exception("signalfd", err);
    }
}

int signal_fd::value_of(std::initializer_list<fd_mode> mode) {
    int res = 0;
    for (auto it = mode.begin(); it != mode.end(); it++) {
        switch (*it) {
            case NONBLOCK:
                res |= SFD_NONBLOCK;
                break;
            case CLOEXEC:
                res |= SFD_CLOEXEC;
                break;
            case SIMPLE:
                res |= 0;
                break;
        }
    }
    return res;
}

socket_wrap::socket_wrap() :
        file_descriptor() {
}

socket_wrap::socket_wrap(int fd) :
        file_descriptor(fd) {
}

socket_wrap::socket_wrap(socket_mode mode) : socket_wrap({mode}) { }

socket_wrap::socket_wrap(std::initializer_list<socket_mode> mode) :
        file_descriptor() {
    int type = SOCK_STREAM | value_of(mode);

    fd = socket(AF_INET, type, 0);

    if (fd == -1) {
        int err = errno;
        throw annotated_exception("socket", err);
    }
}

socket_wrap::socket_wrap(socket_wrap &&other) {
    swap(*this, other);
}

int socket_wrap::value_of(std::initializer_list<socket_mode> modes) const {
    int mode = 0;
    for (auto it = modes.begin(); it != modes.end(); it++) {
        switch (*it) {
            case SIMPLE:
                mode |= 0;
                break;
            case NONBLOCK:
                mode |= O_NONBLOCK;
                break;
            case CLOEXEC:
                mode |= O_CLOEXEC;
                break;
            default:
                mode |= 0;
                break;
        }
    }
    return mode;
}

socket_wrap socket_wrap::accept(socket_mode mode) const {
    int new_fd = ::accept4(fd, 0, 0, value_of({mode}));
    if (new_fd == -1) {
        int err = errno;
        throw annotated_exception("accept", err);
    }
    return socket_wrap(new_fd);
}

socket_wrap socket_wrap::accept(std::initializer_list<socket_mode> mode) const {

    int new_fd = ::accept4(fd, 0, 0, value_of(mode));
    if (new_fd == -1) {
        int err = errno;
        throw annotated_exception("accept", err);
    }
    return socket_wrap(new_fd);
}

void socket_wrap::bind(uint16_t port) const {
    sockaddr_in addr = {};

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr))) {
        int err = errno;
        throw annotated_exception("bind", err);
    }
}

void socket_wrap::connect(endpoint address) const {
    sockaddr_in addr;
    memset(&addr, 0, sizeof addr);

    addr.sin_family = AF_INET;
    addr.sin_port = address.port;
    addr.sin_addr.s_addr = address.ip;

    if (::connect(fd, (struct sockaddr *) (&addr), sizeof(addr))) {
        int err = errno;
        throw annotated_exception("connect", err);
    }
}

void socket_wrap::listen(int max_queue_size) const {
    if (max_queue_size == -1) {
        max_queue_size = SOMAXCONN;
    }

    if (::listen(fd, max_queue_size)) {
        int err = errno;
        throw annotated_exception("listen", err);
    }
}

void socket_wrap::get_option(int name, void *res, socklen_t *res_len) const {
    if (getsockopt(fd, SOL_SOCKET, name, res, res_len) < 0) {
        int err = errno;
        throw annotated_exception("get_option", err);
    }
}


std::string to_string(socket_wrap &wrap) {
    return "socket " + std::to_string(wrap.get());
}


fd_state::fd_state() :
        fd_st(0) {
}

fd_state::fd_state(uint32_t st) :
        fd_st(st) {
}

fd_state::fd_state(state st) :
        fd_state({st}) {
}

fd_state::fd_state(std::initializer_list<state> st) : fd_st(value_of(st)) {

}

fd_state::fd_state(fd_state const &other) :
        fd_st(other.fd_st) {
}

fd_state::fd_state(fd_state &&other) {
    swap(*this, other);
}

fd_state &fd_state::operator=(fd_state other) {
    swap(*this, other);
    return *this;
}

bool fd_state::operator!=(fd_state other) const {
    return fd_st != other.fd_st;
}

bool fd_state::is(fd_state st) const {
    return (fd_st & st.fd_st) != 0;
}

uint32_t fd_state::get() const {
    return fd_st;
}

fd_state operator^(fd_state first, fd_state second) {
    return fd_state(first.get() ^ second.get());
}

fd_state operator|(fd_state first, fd_state second) {
    return fd_state(first.get() | second.get());
}

uint32_t fd_state::value_of(std::initializer_list<state> st) {
    uint32_t res = 0;
    for (auto it = st.begin(); it != st.end(); it++) {
        switch (*it) {
            case IN:
                res |= EPOLLIN;
                break;
            case OUT:
                res |= EPOLLOUT;
                break;
            case ERROR:
                res |= EPOLLERR;
                break;
            case HUP:
                res |= EPOLLHUP;
                break;
            case RDHUP:
                res |= EPOLLRDHUP;
                break;
            default:
                res |= 0;
        }
    }
    return res;
}

bool operator==(fd_state const &first, fd_state const &second) {
    return first.fd_st == second.fd_st;
}

fd_state epoll_registration::get_state() const {
    return events;
}

void swap(fd_state &first, fd_state &second) {
    std::swap(first.fd_st, second.fd_st);
}

epoll_wrap::epoll_wrap(int max_queue_size) :
        file_descriptor(), queue_size(max_queue_size), events(
        new epoll_event[max_queue_size]), handlers{}, started{false}, stopped{
        true} {
    fd = epoll_create(1);
    if (fd == -1) {
        int err = errno;
        throw annotated_exception("epoll_create", err);
    }
}

epoll_wrap::epoll_wrap(epoll_wrap &&other) {
    swap(*this, other);
}

epoll_event epoll_wrap::create_event(int fd,
                                     fd_state const &st) {
    epoll_event event;
    memset(&event, 0, sizeof event);
    event.data.fd = fd;
    event.events = st.get();
    return event;
}

void epoll_wrap::register_fd(const file_descriptor &fd, fd_state events) {
    epoll_event event = create_event(fd.get(), events);

    if (epoll_ctl(this->fd, EPOLL_CTL_ADD, fd.get(), &event)) {
        int err = errno;
        throw annotated_exception("epoll register", err);
    }
}

void epoll_wrap::register_fd(const file_descriptor &fd, fd_state events,
                             handler_t handler) {
    register_fd(fd, events);
    handlers.insert({fd.get(), handler});
}

void epoll_wrap::unregister_fd(const file_descriptor &fd) {
    if (epoll_ctl(this->fd, EPOLL_CTL_DEL, fd.get(), 0)) {
        int err = errno;
        throw annotated_exception("epoll_unregister", err);
    }
    handlers.erase(fd.get());
}

void epoll_wrap::update_fd(const file_descriptor &fd, fd_state events) {
    epoll_event event = create_event(fd.get(), events);
    if (epoll_ctl(this->fd, EPOLL_CTL_MOD, fd.get(), &event)) {
        int err = errno;
        throw annotated_exception("epoll_update", err);
    }
}

void epoll_wrap::update_fd_handler(const file_descriptor &fd, epoll_wrap::handler_t handler) {
    handlers.erase(fd.get());
    handlers.insert({fd.get(), handler});
}

void epoll_wrap::start_wait() {
    if (started) {
        return;
    }
    started = true;
    stopped = false;

    while (!stopped) {
        int events_number = epoll_wait(fd, events.get(), queue_size, -1);
        if (events_number == -1) {
            int err = errno;
            if (err == EINTR) {
                break;
            }
            throw annotated_exception("epoll_wait", err);
        }

        for (int i = 0; i < events_number; i++) {
            int fd = events[i].data.fd;
            uint32_t state = events[i].events;
            handlers_t::iterator it = handlers.find(fd);
            if (it != handlers.end()) {
                handler_t handler = it->second;
                handler(fd_state(state));
            }
            if (stopped) {
                break;
            }
        }
    }
    started = false;
}

void epoll_wrap::stop_wait() {
    stopped = true;
}

void swap(epoll_wrap &first, epoll_wrap &second) {
    using std::swap;
    swap(first.fd, second.fd);
    swap(first.queue_size, second.queue_size);
    swap(first.started, second.started);
    swap(first.stopped, second.stopped);
    swap(first.events, second.events);
    swap(first.handlers, second.handlers);
}

void swap(endpoint &first, endpoint &second) {
    std::swap(first.ip, second.ip);
    std::swap(first.port, second.port);
}

std::string to_string(endpoint const &ep) {
    using std::to_string;
    return to_string(ep.ip) + ":" + to_string(ep.port);
}

epoll_registration::epoll_registration() : epoll(0), fd(0) { }

epoll_registration::epoll_registration(epoll_wrap &epoll, file_descriptor&& fd, fd_state state) :
     epoll(&epoll), fd(std::move(fd)), events(state)
{
    this->epoll->register_fd(this->fd, state);
}

epoll_registration::epoll_registration(epoll_wrap &epoll, file_descriptor&& fd, fd_state state,
                                       epoll_wrap::handler_t handler) : epoll(&epoll), fd(std::move(fd)),
                                                                        events(state) {
    this->epoll->register_fd(this->fd, state, handler);
}

epoll_registration::epoll_registration(epoll_registration &&other) : epoll_registration() {
    swap(*this, other);
}

epoll_registration &epoll_registration::operator=(epoll_registration &&other) {
    swap(*this, other);
    return *this;
}


epoll_registration::~epoll_registration() {
    if (epoll != 0 && fd.get() != 0) {
        epoll->unregister_fd(fd);
    }
}

void epoll_registration::update(fd_state state) {
    if (events != state) {
        events = state;
        epoll->update_fd(fd, state);
    }
}

void epoll_registration::update(epoll_wrap::handler_t handler) {
    epoll->update_fd_handler(fd, handler);
}

void epoll_registration::update(fd_state state, epoll_wrap::handler_t handler) {
    update(state);
    update(handler);
}

file_descriptor &epoll_registration::get_fd() {
    return fd;
}

file_descriptor const &epoll_registration::get_fd() const {
    return fd;
}

void swap(epoll_registration &first, epoll_registration &other) {
    using std::swap;
    swap(first.epoll, other.epoll);
    swap(first.fd, other.fd);
    swap(first.events, other.events);
}


std::string to_string(epoll_registration &er) {
    return "registration " + std::to_string(er.get_fd().get());
}
