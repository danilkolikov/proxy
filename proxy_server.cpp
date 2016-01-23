#include <assert.h>
#include "proxy_server.h"

proxy_server::connection::connection(shared_socket client,
                                     shared_socket server) :
        client(client), server(server), request(), response() {
}

proxy_server::connection::connection(connection const &other) :
        client(other.client), server(other.server), request(other.request), response(
        other.response) {
}

proxy_server::connection::connection(shared_socket client,
                                     shared_socket server, client_request request) :
        client(client), server(server), request(request), response() {
}

proxy_server::connection::connection(shared_socket client) :
        client(client), server(nullptr), request(), response() { }

proxy_server::connection &proxy_server::connection::operator=(connection other) {
    swap(*this, other);
    return *this;
}

socket_wrap const &proxy_server::connection::get_client() const {
    return *client;
}

socket_wrap const &proxy_server::connection::get_server() const {
    return *server;
}

client_request &proxy_server::connection::get_request() {
    return request;
}

server_response &proxy_server::connection::get_response() {
    return response;
}

void proxy_server::connection::clean_request() {
    request = client_request();
}

void proxy_server::connection::clean_response() {
    response = server_response();
}

bool proxy_server::connection::established() const {
    return client_connected() && server_connected();
}

void proxy_server::connection::set_server(shared_socket server) {
    this->server = server;
}

shared_socket proxy_server::connection::get_shared_server() const {
    return server;
}

void proxy_server::connection::set_response(server_response response) {
    this->response = response;
}

void proxy_server::connection::reset_server() {
    server.reset();
}


void proxy_server::connection::reset_client() {
    client.reset();
}


bool proxy_server::connection::client_connected() const {
    return client != nullptr;
}

bool proxy_server::connection::server_connected() const {
    return server != nullptr;
}

shared_socket proxy_server::connection::get_shared_client() const {
    return client;
}

void proxy_server::connection::set_client(shared_socket client) {
    this->client = client;
}

void proxy_server::connection::set_request(client_request request) {
    this->request = request;
}

void swap(proxy_server::connection &first, proxy_server::connection &second) {
    using std::swap;
    swap(first.client, second.client);
    swap(first.server, second.server);
    swap(first.request, second.request);
    swap(first.response, second.response);
}


std::string to_string(proxy_server::connection const &conn) {
    std::string res = "connection ";
    if (!conn.client_connected()) {
        res += "X";
    } else {
        res += std::to_string(conn.get_client().get());
    }
    res += " <-> ";
    if (!conn.server_connected()) {
        res += " X";
    } else {
        res += std::to_string(conn.get_server().get());
    }
    return res;
}

proxy_server::proxy_server(shared_epoll s_epoll, uint16_t port, int queue_size) : epoll(s_epoll), ticks(0), sockets() {
    shared_socket listener = std::make_shared<socket_wrap>(socket_wrap(socket_wrap::NONBLOCK));
    shared_event_fd notifier = std::make_shared<event_fd>(event_fd(0, event_fd::SEMAPHORE));
    shared_timer_fd timer = std::make_shared<timer_fd>(timer_fd(timer_fd::MONOTONIC, timer_fd::SIMPLE));

    listener->bind(port);
    listener->listen(queue_size);
    timer->set_interval(TICK_INTERVAL, TICK_INTERVAL);

    rt = unique_resolver(new resolver_t(notifier));

    epoll_wrap::handler_t listener_handler = [this, listener](fd_state state, epoll_wrap &epoll1) {
        if (state.is(fd_state::IN)) {
            shared_socket client = listener->accept(socket_wrap::NONBLOCK);
            shared_connection conn = std::make_shared<connection>(connection(client));

            log("new client accepted", client->get());
            save_registration(std::move(epoll_registration(epoll, *client, fd_state::IN,
                                                           proxy_server::make_new_client_handler(conn, ""))),
                              LONG_SOCKET_TIMEOUT);
        }
    };

    epoll_wrap::handler_t notifier_handler = [this, notifier](fd_state state, epoll_wrap &epoll1) {
        if (state.is(fd_state::IN)) {
            uint64_t u;
            notifier->read(&u, sizeof(uint64_t));
            shared_socket destination(new socket_wrap(socket_wrap::NONBLOCK));

            resolved_ip_t ip = rt->get_ip();

            log(*ip.get_extra().conn, "ip resolved: " + to_string(ip.get_ip()));

            try {
                destination->connect(ip.get_ip());
            } catch (annotated_exception const &e) {
                if (e.get_errno() != EINPROGRESS) {
                    log(e);
                    return;
                }
            }

            ip.get_extra().conn->set_server(destination);

            save_registration(epoll_registration(epoll, *destination, fd_state::OUT,
                                                 proxy_server::make_server_connect_handler(ip)),
                              SHORT_SOCKET_TIMEOUT);
        }
    };

    epoll_wrap::handler_t timer_handler = [this, timer](fd_state state, epoll_wrap &wrap) {
        if (state.is(fd_state::IN)) {
            uint64_t ticked = 0;
            timer->read(&ticked, sizeof ticked);


            std::string active = "";
            ticks += ticked;
            for (auto it = sockets.begin(); it != sockets.end(); it++) {
                if (it->second.expires_in <= ticks) {
                    sockets.erase(it);
                    log("socket " + std::to_string(it->second.registration.get()), "closed due timeout");
                } else {
                    active.append(std::to_string(it->second.registration.get()));
                    active.append(" ");
                }
            }

            log("active connections", active);
        }
    };

    save_registration(epoll_registration(epoll, *listener, fd_state::IN, listener_handler), INFINITE_TIMEOUT);
    save_registration(epoll_registration(epoll, *notifier, fd_state::IN, notifier_handler), INFINITE_TIMEOUT);
    save_registration(epoll_registration(epoll, *timer, fd_state::IN, timer_handler), INFINITE_TIMEOUT);
}


epoll_wrap::handler_t proxy_server::make_new_client_handler(shared_connection connect, std::string old_host) {
    return [this, connect, old_host](fd_state state, epoll_wrap &epoll) mutable {
        connection &conn = *connect;
        set_active(conn.get_client());

        if (state.is(fd_state::RDHUP)) {
            if (!conn.get_client().can_read()) {
                log(conn, "client dropped connection, closing");
                close(conn.get_client());
                conn.reset_client();
                return;
            }
        }

        if (state.is(fd_state::IN)) {
            conn.get_request().read_from(conn.get_client());

            if (conn.get_request().is_header_read()) {

                std::string host = conn.get_request().get_header().get_property("host");
                if (!old_host.empty()) {
                    log(conn, "reused: " + old_host + " -> " + host);
                }

                bool host_changed = old_host.compare(host) != 0;

                if (host_changed) {
                    if (conn.server_connected()) {
                        close(conn.get_server());
                        conn.reset_server();
                    }
                }

                if (conn.get_request().get_header().get_request_line().get_type() == request_line::GET) {
                    bool found = is_cached(conn.get_request().get_header());

                    // Validate cached responses
                    if (found) {
                        log(conn, "found cached for " + to_url(conn.get_request().get_header()) + ", validating...");
                        conn.set_response(get_cached(conn.get_request().get_header()));

                        shared_connection validate = std::make_shared<connection>(
                                connection(0, 0,
                                           make_validate_request(conn.get_request().get_header(),
                                                                 conn.get_response().get_header())));

                        action do_after_connection = [this, connect, validate]() {
                            if (connect->client_connected()) {
                                // Load response
                                get_registration(validate->get_server()).update(fd_state::OUT,
                                                                                make_cache_validate_handler(
                                                                                        connect, validate));
                                // And wait
                                get_registration(connect->get_client()).update(fd_state::WAIT);
                            } else {
                                close(*connect);
                                close(*validate);
                            }
                        };

                        if (conn.established()) {
                            validate->set_server(conn.get_shared_server());
                            conn.reset_server();
                            do_after_connection();
                        } else {
                            conn.reset_server();
                            connect_to_server(validate, [do_after_connection]() {
                                do_after_connection();
                            });
                        }
                        return;
                    }
                }

                if (host.compare(old_host) == 0 && registered(conn.get_server())) {
                    get_registration(conn.get_server()).update(fd_state::OUT);
                    if (conn.get_request().can_read()) {
                        get_registration(conn.get_client()).update(make_client_handler(connect));
                    } else {
                        get_registration(conn.get_client()).update(fd_state::WAIT, make_client_handler(connect));
                    }
                } else {
                    if (conn.established()) {
                        close(conn.get_server());
                        conn.reset_server();
                    }
                    get_registration(conn.get_client()).update(fd_state::WAIT);

                    connect_to_server(connect, start_data_transfer(connect));
                }
            }
        }
    };
}

epoll_wrap::handler_t proxy_server::make_server_connect_handler(resolved_ip_t s_ip) {
    return [this, s_ip](fd_state state, epoll_wrap &epoll) mutable {
        shared_connection const &s_connection = s_ip.get_extra().conn;
        connection &conn = *s_connection;
        set_active(conn.get_server());

        if (state.is(fd_state::RDHUP)) {
            close(conn);
            log(conn, "server dropped connection, closing");
            return;
        }

        if (state.is({fd_state::HUP, fd_state::ERROR})) {
            int code;
            socklen_t size = sizeof(code);
            conn.get_server().get_option(SO_ERROR, &code, &size);
            switch (code) {
                case ENETUNREACH:
                case ECONNREFUSED: {
                    endpoint old_ip = s_ip.get_ip();
                    s_ip.next_ip();
                    log(conn, "ip " + to_string(old_ip) + " isn't valid. Trying " + to_string(s_ip.get_ip()));
                    endpoint ip = s_ip.get_ip();
                    if (ip.ip == 0) {
                        log(conn, "no relevant ip, closing...");
                        close(conn);
                        return;
                    }
                    try {
                        conn.get_server().connect(ip);
                    } catch (annotated_exception const &e) {
                        if (e.get_errno() != EINPROGRESS) {
                            log(e);
                            // bad error
                            close(conn);
                            return;
                        }
                    }
                    return;
                }
                case EPIPE:
                    close(conn);
                    return;
            }
            throw annotated_exception("server connect", code);
        }

        if (state.is(fd_state::OUT)) {
            log(conn, "established");
            change_timeout(conn.get_server(), LONG_SOCKET_TIMEOUT);

            s_ip.get_extra().do_next();
        }
    };
}

epoll_wrap::handler_t proxy_server::make_server_handler(
        shared_connection connect) {
    return [this, connect](fd_state state, epoll_wrap &epoll) {
        connection &conn = *connect;
        set_active(conn.get_server());

        if (!conn.client_connected()) {
            log(conn, "client dropped connection, closing");
            close(conn);
            return;
        }

        if (state.is(fd_state::RDHUP)) {
            // Drop if has nothing to read
            if (conn.get_server().can_read() == 0) {
                log(conn, "server dropped connection, closing");
                close(conn);
                return;
            }
        }

        if (state.is({fd_state::HUP, fd_state::ERROR})) {
            int code;
            socklen_t size = sizeof(code);
            conn.get_server().get_option(SO_ERROR, &code, &size);
            annotated_exception exception("server", code);
            switch (code) {
                case EPIPE:
                case ECONNRESET:
                    log(exception);
                    close(conn);
                    return;
            }

            if (code != 0) {
                throw exception;
            }
            return;
        }

        if (state.is(fd_state::OUT) && conn.get_request().can_write()) {
            try {
                conn.get_request().write_to(conn.get_server());
            } catch (annotated_exception const &e) {
                log(e);

                if (e.get_errno() == ETIMEDOUT || e.get_errno() == EPIPE) {
                    close(conn);
                    return;
                }
            }

            if (conn.get_request().is_written()) {
                log(conn, "client's request sent");
                conn.clean_response();
                get_registration(conn.get_server()).update(fd_state::IN);
            } else {
                if (conn.get_request().can_read()) {
                    get_registration(conn.get_client()).update(fd_state::IN);
                }
                if (!conn.get_request().can_write()) {
                    get_registration(conn.get_server()).update(fd_state::WAIT);
                }
            }
            return;
        }

        if (state.is(fd_state::IN) && conn.get_response().can_read()) {
            conn.get_response().read_from(conn.get_server());

            if (conn.get_response().can_write()) {
                get_registration(conn.get_client()).update(fd_state::OUT);
            }
            if (!conn.get_response().can_read()) {
                get_registration(conn.get_server()).update(fd_state::WAIT);
            }
            if (conn.get_response().is_read()) {
                log(conn, "server's response read");

                if (sholud_cache(conn.get_response().get_header())) {
                    save_cached(conn.get_request().get_header(), conn.get_response().get_cache());
                }
                if (conn.get_response().get_header().has_property("connection") &&
                    to_lower(conn.get_response().get_header().get_property("connection")).compare("close") == 0) {

                    close(conn.get_server());
                    conn.reset_server();

                    log(conn, "server closed due to \"Connection = close\" ");
                } else {
                    get_registration(conn.get_server()).update(fd_state::WAIT);
                }
            }
            return;
        }

    };
}

epoll_wrap::handler_t proxy_server::make_client_handler(
        shared_connection connect) {
    return [this, connect](fd_state state, epoll_wrap &epoll) {
        connection &conn = *connect;
        set_active(conn.get_client());

        if (state.is(fd_state::RDHUP)) {
            log(conn, "client dropped connection, closing");
            close(conn);
            return;
        }

        if (state.is({fd_state::HUP, fd_state::ERROR})) {
            int code;
            socklen_t size = sizeof(code);
            conn.get_client().get_option(SO_ERROR, &code, &size);
            annotated_exception exception("client", code);
            switch (code) {
                case ECONNRESET:
                case EPIPE:
                    log(exception);
                    close(conn);
                    return;
            }
            if (code != 0) {
                throw exception;
            }
        }

        if (state.is(fd_state::IN) && conn.get_request().can_read()) {
            conn.get_request().read_from(conn.get_client());
            if (conn.established() && conn.get_request().can_write()) {
                get_registration(conn.get_server()).update(fd_state::OUT);
            }
            if (!conn.get_request().can_read()) {
                get_registration(conn.get_client()).update(fd_state::WAIT);
            }
            if (conn.get_request().is_read()) {
                log(conn, "client's request read");
            }
            return;
        }

        if (state.is(fd_state::OUT) && conn.get_response().can_write()) {
            try {
                conn.get_response().write_to(conn.get_client());
            } catch (annotated_exception const &e) {
                log(e);

                if (e.get_errno() == ETIMEDOUT || e.get_errno() == EPIPE) {
                    close(conn);
                    return;
                }
            }


            if (conn.established() && conn.get_response().can_read()) {
                get_registration(conn.get_server()).update(fd_state::IN);
            }
            if (!conn.get_response().can_write()) {
                get_registration(conn.get_client()).update(fd_state::WAIT);
            }

            if (conn.get_response().is_written()) {
                log(conn, "server response sent");

                if (conn.get_response().get_header().has_property("connection") &&
                    to_lower(conn.get_response().get_header().get_property("connection")).compare("close") == 0) {
                    close(conn.get_client());
                    conn.reset_client();

                    log(conn, "client closed due to \"Connection = close\" ");
                } else {
                    std::string old_host = conn.get_request().get_header().get_property("host");
                    conn.clean_request();
                    conn.clean_response();
                    get_registration(conn.get_server()).update(fd_state::WAIT);
                    get_registration(conn.get_client()).update(fd_state::IN,
                                                               proxy_server::make_new_client_handler(connect,
                                                                                                     old_host));

                    log(conn, "kept alive");

                }
            }
        }
    };
}

epoll_wrap::handler_t proxy_server::make_cache_validate_handler(shared_connection old_conn,
                                                                shared_connection validate) {
    return [this, old_conn, validate](fd_state state, epoll_wrap &epoll) {
        connection &conn = *validate;

        if (state.is(fd_state::RDHUP)) {
            if (conn.get_server().can_read() == 0) {
                old_conn->set_server(conn.get_shared_server());
                log(*old_conn, "server dropped connection, closing");

                close(conn.get_server());
                conn.reset_server();

                log(*old_conn, "cache validation failed");
                // If failed, just download again
                delete_cached(old_conn->get_request().get_header());
                old_conn->clean_response();
                connect_to_server(old_conn, start_data_transfer(old_conn));

                return;
            }
        }

        if (state.is(fd_state::OUT)) {
            try {
                conn.get_request().write_to(conn.get_server());
            } catch (annotated_exception const &e) {
                log(e);

                if (e.get_errno() == ETIMEDOUT || e.get_errno() == EPIPE) {
                    close(conn);
                    return;
                }
            }

            if (conn.get_request().is_written()) {
                log(*old_conn, "validation request sent");
                get_registration(conn.get_server()).update(fd_state::IN);
            }
            return;
        }

        if (state.is(fd_state::IN) && conn.get_response().can_read()) {
            conn.get_response().read_from(conn.get_server());

            if (conn.get_response().is_read()) {
                if (conn.get_response().get_header().has_property("connection") &&
                    to_lower(conn.get_response().get_header().get_property("connection")).compare("close") == 0) {
                    close(conn.get_server());
                    conn.reset_server();

                    log(conn, "server closed due to \"Connection = close\" ");
                }

                int code = conn.get_response().get_header().get_request_line().get_code();
                old_conn->set_server(conn.get_shared_server());
                conn.reset_server();

                if (code == 200 || code == 304) {
                    // Can send cached
                    log(*old_conn, "cache valid");
                    send_cached(old_conn)();

                } else {
                    // Can't do it
                    log(*old_conn, "cache invalid");
                    delete_cached(old_conn->get_request().get_header());
                    old_conn->clean_response();
                    if (old_conn->established()) {
                        // If server still connected, send data
                        start_data_transfer(old_conn)();
                    } else {
                        // Otherwise, connect again
                        connect_to_server(old_conn, start_data_transfer(old_conn));
                    }
                }
            }
            return;
        }

    };
}

proxy_server::action proxy_server::start_data_transfer(shared_connection connect) {
    return [this, connect]() {
        connection &conn = *connect;
        if (conn.established()) {
            get_registration(conn.get_server()).update(fd_state::OUT, make_server_handler(connect));
            get_registration(conn.get_client()).update(fd_state::WAIT, make_client_handler(connect));
        } else {
            close(conn);
        }
    };
}

proxy_server::action proxy_server::send_cached(shared_connection connect) {
    return [this, connect] {
        connection &conn = *connect;
        if (conn.client_connected()) {
            get_registration(conn.get_client()).update(fd_state::OUT, make_client_handler(connect));
        } else {
            close(conn);
        }
        if (conn.server_connected()) {
            get_registration(conn.get_server()).update(fd_state::WAIT, make_server_handler(connect));
        }
    };
}

void proxy_server::connect_to_server(shared_connection conn, action do_next) {
    log(*conn, "establishing connection to " + conn->get_request().get_header().get_property("host"));
    rt->resolve_url(conn->get_request().get_header().get_property("host"), resolver_extra(conn, do_next));
}

void proxy_server::close(socket_wrap const &socket) {
    sockets.erase(socket.get());
}

void proxy_server::close(connection const &conn) {
    if (conn.client_connected()) {
        close(conn.get_client());
    }
    if (conn.server_connected()) {
        close(conn.get_server());
    }
}

epoll_registration &proxy_server::get_registration(file_descriptor const &socket) {
    return sockets.find(socket.get())->second.registration;
}

void proxy_server::save_registration(epoll_registration registration, size_t timeout) {
    int fd = registration.get();
    safe_registration sf(std::move(registration), timeout, ticks);
    sockets.insert(std::make_pair(std::move(fd), std::move(sf)));
}

bool proxy_server::registered(file_descriptor const &fd) {
    return sockets.find(fd.get()) != sockets.end();
}

proxy_server::safe_registration::safe_registration() : registration(), timeout(0), expires_in(0) {
}

proxy_server::safe_registration::safe_registration(epoll_registration registration, size_t timeout, size_t ticks) :
        registration(std::move(registration)), timeout(timeout), expires_in(ticks + timeout) {
}

proxy_server::safe_registration::safe_registration(proxy_server::safe_registration &&other) : safe_registration() {
    swap(*this, other);
}

proxy_server::safe_registration &proxy_server::safe_registration::operator=(
        proxy_server::safe_registration &&other) {
    swap(*this, other);
    return *this;
}

void swap(proxy_server::safe_registration &first, proxy_server::safe_registration &second) {
    using std::swap;
    swap(first.registration, second.registration);
    swap(first.timeout, second.timeout);
    swap(first.expires_in, second.expires_in);
}

void proxy_server::change_timeout(file_descriptor const &socket, size_t socket_timeout) {
    auto it = sockets.find(socket.get());
    if (it == sockets.end()) {
        return;
    }
    it->second.timeout = socket_timeout;
    it->second.expires_in = ticks + socket_timeout;
}

void proxy_server::set_active(file_descriptor const &socket) {
    auto it = sockets.find(socket.get());
    if (it == sockets.end()) {
        return;
    }
    if (it->second.timeout == INFINITE_TIMEOUT) {
        return;
    }
    it->second.expires_in = ticks + it->second.timeout;
}

std::string proxy_server::to_url(request_header const &request) const {
    std::string url = request.get_property("host");
    url += request.get_request_line().get_url();
    return url;
}

void proxy_server::save_cached(request_header const &request, cached_message const &response) {
    std::string url = to_url(request);
    auto it = cache.find(url);
    if (it == cache.end()) {
        cache.insert({url, response});
    }
}

bool proxy_server::is_cached(request_header const &request) const {
    auto it = cache.find(to_url(request));
    return it != cache.end();
}

cached_message proxy_server::get_cached(request_header const &request) const {
    return cache.find(to_url(request))->second;
}

void proxy_server::delete_cached(request_header const &request) {
    auto it = cache.find(to_url(request));
    if (it != cache.end()) {
        cache.erase(it);
    }
}

client_request proxy_server::make_validate_request(request_header rqst, response_header response) const {
    request_header header(rqst.get_request_line());
    header.set_property("host", rqst.get_property("host"));
    if (response.has_property("etag")) {
        header.set_property("if-none-match", response.get_property("etag"));
    }
    if (response.has_property("last-modified")) {
        header.set_property("if-modified-since", response.get_property("last-modified"));
    }
    return client_request(header, "");
}

bool proxy_server::sholud_cache(response_header const &header) const {
    if (header.has_property("cache-control")) {
        std::string value = to_lower(header.get_property("cache_control"));
        if (value.find("no-cache") != std::string::npos ||
            value.find("no-store") != std::string::npos ||
            value.find("must-revalidate") != std::string::npos ||
            value.find("proxy-revalidate") != std::string::npos ||
            value.find("max-age=0") != std::string::npos) {
            return false;
        }
    }

    if (header.has_property("pragma")) {
        std::string value = to_lower(header.get_property("pragma"));
        if (value.find("no-cache") != std::string::npos) {
            return false;
        }
    }

    if (header.has_property("cache") &&
        to_lower(header.get_property("cache")).compare("none") == 0) {
        return false;
    }

    if (!header.has_property("etag") && !header.has_property("last-modified")) {
        return false;       // Otherwise can't validate
    }

    return true;
}

