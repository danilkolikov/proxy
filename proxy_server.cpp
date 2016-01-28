#include "proxy_server.h"

proxy_server::proxy_server(epoll_wrap &s_epoll, resolver_t &rt, uint16_t port, int queue_size) :
        epoll(s_epoll), rt(rt), ticks(0) {

    socket_wrap listener(socket_wrap::NONBLOCK);
    event_fd notifier(0, event_fd::SEMAPHORE);
    timer_fd timer(timer_fd::MONOTONIC, timer_fd::SIMPLE);

    listener.bind(port);
    listener.listen(queue_size);
    timer.set_interval(TICK_INTERVAL, TICK_INTERVAL);

    epoll_wrap::handler_t listener_handler = [this](fd_state state) {
        if (state.is(fd_state::IN)) {
            socket_wrap &listener = *static_cast<socket_wrap *>(&this->listener->second.get_fd());
            socket_wrap client = listener.accept(socket_wrap::NONBLOCK);

            log("new client accepted", client.get());
            sockets_t::iterator it = save_registration(epoll_registration(epoll, std::move(client), fd_state::IN),
                                                       LONG_SOCKET_TIMEOUT);
            read(it->second, client_request(), it, first_request_read(it));
        }
    };

    epoll_wrap::handler_t notifier_handler = [this](fd_state state) {
        if (state.is(fd_state::IN)) {
            uint64_t u;
            file_descriptor &notifier = this->notifier->second.get_fd();
            notifier.read(&u, sizeof(uint64_t));

            socket_wrap destination(socket_wrap::NONBLOCK);

            resolved_ip_t ip = this->rt.get_ip();

            on_resolve_t::iterator it = on_resolve.find({ip.get_extra().socket, ip.get_extra().host});
            sockets_t::iterator client = sockets.find(ip.get_extra().socket);
            if (it == on_resolve.end()) {
                // Client disconnected during resolving of ip
                log("client " + std::to_string(ip.get_extra().socket), "Client disconnected during resolving of ip");
                return;
            }

            try {
                destination.connect(ip.get_ip());
            } catch (annotated_exception const &e) {
                if (e.get_errno() != EINPROGRESS) {
                    log(e);
                    sockets.erase(client);
                    on_resolve.erase(it);
                    return;
                }
            }

            connection conn(std::move(client->second),
                            epoll_registration(epoll, std::move(destination), fd_state::OUT),
                            SHORT_SOCKET_TIMEOUT, ticks);

            sockets.erase(client);
            log(conn, "ip for " + ip.get_extra().host + " resolved: " + to_string(ip.get_ip()));

            connections_t::iterator conn_it = save_connection(std::move(conn));

            conn_it->get_client_registration()
                    .update(fd_state::RDHUP,
                            [this, it, conn_it](fd_state state) {
                                // If client disconnect while we haven't connected to server
                                if (state.is(fd_state::RDHUP)) {
                                    log(*conn_it, "client dropped connection");
                                    on_resolve.erase(it);
                                    close(conn_it);
                                }
                            });

            conn_it->get_server_registration().update(make_server_connect_handler(conn_it, ip, it));
        }
    };

    epoll_wrap::handler_t timer_handler = [this](fd_state state) {
        if (state.is(fd_state::IN)) {
            file_descriptor &timer = this->timer->second.get_fd();
            uint64_t ticked = 0;
            timer.read(&ticked, sizeof ticked);

            ticks += ticked * TICK_INTERVAL;
            for (auto it = sockets.begin(); it != sockets.end();) {
                if (it->second.expires_in <= ticks) {
                    log(it->second.get_fd(), "closed due timeout");
                    it = sockets.erase(it);
                } else {
                    it++;
                }
            }

            ticks += ticked;
            for (auto it = connections.begin(); it != connections.end();) {
                if (it->expires_in <= ticks) {
                    log(*it, "closed due timeout");
                    it = connections.erase(it);
                } else {
                    it++;
                }
            }
        }
    };

    this->listener = save_registration(epoll_registration(epoll, std::move(listener), fd_state::IN, listener_handler),
                                       INFINITE_TIMEOUT);
    this->notifier = save_registration(epoll_registration(epoll, std::move(notifier), fd_state::IN, notifier_handler),
                                       INFINITE_TIMEOUT);
    this->timer = save_registration(epoll_registration(epoll, std::move(timer), fd_state::IN, timer_handler),
                                    INFINITE_TIMEOUT);
}


proxy_server::action_with_request proxy_server::first_request_read(sockets_t::iterator client) {
    return [this, client](client_request rqst) {

        std::string host = rqst.get_header().get_property("host");
        connect_to_server(client, host, handle_client_request(rqst));
    };
}


void proxy_server::connect_to_server(sockets_t::iterator sock, std::string host, action_with_connection do_next) {
    socket_wrap &s = *static_cast<socket_wrap *>(&sock->second.get_fd());
    log(s, "establishing connection to " + host);
    on_resolve.insert({{s.get(), host}, do_next});

    // If socket disconnected during resolving, stop resolving
    sock->second.update(fd_state::RDHUP, [this, sock, host](fd_state state) {
        if (state.is(fd_state::RDHUP)) {
            log(sock->second.get_fd(), "disconnected during resolving of IP");
            on_resolve.erase(on_resolve.find({sock->second.get_fd().get(), host}));
            close(sock);
        }
    });

    rt.resolve_host(host, notifier->second.get_fd(), {s.get(), host});
}

proxy_server::action_with_connection proxy_server::handle_client_request(client_request rqst) {
    return [this, rqst](connections_t::iterator conn) {
        if (rqst.get_header().get_request_line().get_type() == request_line::GET) {

            // If cached, validate
            if (is_cached(rqst.get_header())) {
                log(*conn, "found cached for " + to_url(rqst.get_header()) + ", validating...");

                server_response cached = get_cached(rqst.get_header());
                send_and_read(conn->get_server_registration(),
                              make_validate_request(rqst.get_header(), cached.get_header()),
                              conn, handle_validation_response(conn, rqst, cached));
                return;
            }
        }
        fast_transfer(conn, rqst);
    };
}

proxy_server::action_with_response proxy_server::handle_validation_response(connections_t::iterator conn,
                                                                            client_request rqst,
                                                                            server_response cached) {
    return [this, rqst, conn, cached](server_response resp) mutable {
        int code = resp.get_header().get_request_line().get_code();

        if (code == 200 || code == 304) {
            // Can send cached
            log(*conn, "cache valid");
            if (resp.get_header().has_property("connnection")) {
                cached.get_header().set_property("connection", resp.get_header().get_property("connection"));
            }

            send_server_response(conn, std::move(rqst), std::move(cached));
        } else {
            // Can't do it
            log(*conn, "cache invalid");

            delete_cached(rqst.get_header());

            // Send data to server
            if (resp.get_header().has_property("connection") &&
                to_lower(resp.get_header().get_property("connection")).compare("close") == 0) {
                // Re-connect if closed
                log(*conn, "server closed due to \"Connection = close\", reconnecting");
                epoll_registration client = std::move(conn->get_client_registration());
                close(conn);
                sockets_t::iterator it = save_registration(std::move(client), LONG_SOCKET_TIMEOUT);

                connect_to_server(it,
                                  rqst.get_header().get_property("host"),
                                  [this, rqst](connections_t::iterator conn) {
                                      fast_transfer(conn, rqst);
                                  });
            } else {
                fast_transfer(conn, rqst);
            }
        }
    };
}


proxy_server::action proxy_server::reuse_connection(connections_t::iterator conn, std::string old_host) {
    return [this, conn, old_host]() {
        log(*conn, "server response sent");
        log(*conn, "kept alive");

        conn->get_server_registration().update(fd_state::RDHUP, [this, conn](fd_state state) {
            // It's a rare situation when server that keeps connection alive drops it
            if (state.is(fd_state::RDHUP)) {
                log(*conn, "server dropped connection");
                epoll_registration client = std::move(conn->get_client_registration());
                close(conn);

                sockets_t::iterator it = save_registration(std::move(client), LONG_SOCKET_TIMEOUT);
                read(it->second, client_request(), it, first_request_read(it));
            }
        });

        // Read client request
        read < request_header, connections_t::iterator > (conn->get_client_registration(), client_request(), conn,
                [this, conn, old_host](client_request rqst) {

                    std::string host = rqst.get_header().get_property("host");
                    log(*conn, "client reused: " + old_host + " -> " + host);

                    if (host.compare(old_host) == 0) {
                        // If host the same, send request
                        handle_client_request(std::move(rqst))(conn);
                    } else {
                        // Otherwise, disconnect, connect and send
                        log(*conn, "disconnect from " + old_host);
                        epoll_registration client = std::move(conn->get_client_registration());
                        close(conn);

                        sockets_t::iterator it = save_registration(std::move(client), LONG_SOCKET_TIMEOUT);
                        connect_to_server(it, host, handle_client_request(std::move(rqst)));
                    }
                });

    };
}


epoll_wrap::handler_t proxy_server::make_server_connect_handler(connections_t::iterator conn, resolved_ip_t ip,
                                                                on_resolve_t::iterator query) {
    return [this, conn, ip, query](fd_state state) mutable {
        socket_wrap const &server = conn->get_server();
        set_active(conn);

        if (state.is(fd_state::RDHUP)) {
            log(*conn, "connection to " + ip.get_extra().host +
                       ": server " + std::to_string(server.get()) + "dropped connection");
            on_resolve.erase(query);
            close(conn);
            return;
        }

        if (state.is({fd_state::HUP, fd_state::ERROR})) {
            int code;
            socklen_t size = sizeof(code);
            server.get_option(SO_ERROR, &code, &size);

            annotated_exception e("connect", code);
            switch (code) {
                case ENETUNREACH:
                case ECONNREFUSED: {
                    if (!ip.has_ip()) {
                        log(*conn, "connection to " + ip.get_extra().host + ": no relevant ip, closing");
                        on_resolve.erase(query);
                        close(conn);
                        return;
                    }
                    endpoint old_ip = ip.get_ip();
                    ip.next_ip();
                    log(*conn, "connection to " + ip.get_extra().host + ": ip "
                               + to_string(old_ip) + " isn't valid. Trying " + to_string(ip.get_ip()));
                    endpoint cur_ip = ip.get_ip();

                    try {
                        server.connect(cur_ip);
                    } catch (annotated_exception const &e) {
                        if (e.get_errno() != EINPROGRESS) {
                            // bad error
                            log(e);
                            log(*conn, "closing");
                            on_resolve.erase(query);
                            close(conn);
                            return;
                        }
                    }
                    return;
                }
                case EPIPE:
                    log(e);
                    on_resolve.erase(query);
                    close(conn);
                    return;
            }
            throw e;
        }

        if (state.is(fd_state::OUT)) {
            log(*conn, "established");

            action_with_connection action = query->second;
            on_resolve.erase(query);

            conn->get_client_registration().update(fd_state::WAIT);
            conn->get_server_registration().update(fd_state::WAIT);
            change_timeout(conn, LONG_SOCKET_TIMEOUT);

            action(conn);
        }
    };
}

template<typename T, typename C>
void proxy_server::read(epoll_registration &from, buffered_message<T> message,
                        C iterator, action_with<buffered_message<T>> next) {
    std::shared_ptr<buffered_message<T>> s_message = std::make_shared<buffered_message<T>>(std::move(message));
    from.update({fd_state::IN, fd_state::RDHUP},
                [this, &from, s_message, iterator, next](fd_state state) {
                    file_descriptor const &fd = from.get_fd();
                    set_active(iterator);

                    if (state.is(fd_state::RDHUP)) {
                        if (fd.can_read() == 0) {
                            log(fd, "disconnected");
                            close(iterator);
                            return;
                        }
                    }

                    if (state.is({fd_state::HUP, fd_state::ERROR})) {
                        int code;
                        socklen_t size = sizeof(code);
                        socket_wrap const &sock = *static_cast<socket_wrap const *>(&fd);
                        sock.get_option(SO_ERROR, &code, &size);
                        annotated_exception exception(to_string(sock) + " read", code);
                        switch (code) {
                            case ECONNRESET:
                            case EPIPE:
                                log(exception);
                                close(iterator);
                                return;
                        }
                        if (code != 0) {
                            throw exception;
                        }
                    }

                    if (state.is(fd_state::IN)) {

                        s_message->read_from(fd);

                        if (s_message->is_read()) {
                            from.update(fd_state::WAIT);
                            next(std::move(*s_message));
                        }
                    }
                });
}

template<typename T, typename C>
void proxy_server::send(epoll_registration &to, buffered_message<T> message,
                        C iterator, action next) {
    std::shared_ptr<buffered_message<T>> s_message = std::make_shared<buffered_message<T>>(std::move(message));
    to.update({fd_state::OUT, fd_state::RDHUP},
              [this, &to, s_message, iterator, next](fd_state state) {
                  file_descriptor const &fd = to.get_fd();
                  set_active(iterator);

                  if (state.is(fd_state::RDHUP)) {
                      log(fd, "disconnected");
                      close(iterator);
                      return;
                  }

                  if (state.is({fd_state::HUP, fd_state::ERROR})) {
                      int code;
                      socklen_t size = sizeof(code);
                      socket_wrap const &sock = *static_cast<socket_wrap const *>(&fd);
                      sock.get_option(SO_ERROR, &code, &size);
                      annotated_exception exception(to_string(sock) + " send", code);
                      switch (code) {
                          case ECONNRESET:
                          case EPIPE:
                              log(exception);
                              close(iterator);
                              return;
                      }
                      if (code != 0) {
                          throw exception;
                      }
                  }

                  if (state.is(fd_state::OUT)) {
                      s_message->write_to(fd);

                      if (s_message->is_written()) {
                          to.update(fd_state::WAIT);
                          next();
                      }
                  }
              });
}

template<typename C>
void proxy_server::send_and_read(epoll_registration &to, client_request rqst,
                                 C iterator, action_with_response next) {
    send(to, rqst, iterator, [this, &to, iterator, next]() {
        log(*iterator, "request sent");
        read(to, server_response(), iterator, next);
    });
}

void proxy_server::send_server_response(connections_t::iterator conn, client_request rqst, server_response resp) {
    log(*conn, "server's response read");
    bool closed = resp.get_header().has_property("connection") &&
                  to_lower(resp.get_header().get_property("connection")).compare("close") == 0;

    if (closed) {
        log(*conn, "server closed due to \"Connection = close\" ");
        epoll_registration client = std::move(conn->get_client_registration());
        sockets_t::iterator it = save_registration(std::move(client), LONG_SOCKET_TIMEOUT);
        close(conn);

        send(it->second, resp, it, [this, it]() {
            log(it->second.get_fd(), "server response sent");
            log(it->second.get_fd(), "closed due to \"Connection = close\"");
            close(it);
        });
    } else {
        conn->get_server_registration().update(fd_state::WAIT);
        send(conn->get_client_registration(), std::move(resp), conn,
             reuse_connection(conn, rqst.get_header().get_property("host")));
    }

}

void proxy_server::fast_transfer(connections_t::iterator conn, client_request rqst) {
    send(conn->get_server_registration(), rqst, conn, [this, conn, rqst]() {
        std::shared_ptr<client_request> s_rqst = std::make_shared<client_request>(std::move(rqst));
        std::shared_ptr<server_response> resp = std::make_shared<server_response>(server_response());

        conn->get_server_registration().update(
                {fd_state::IN, fd_state::RDHUP}, [this, conn, s_rqst, resp](fd_state state) {
            set_active(conn);
            file_descriptor const &server = conn->get_server();

            if (state.is(fd_state::RDHUP)) {
                if (server.can_read() == 0) {
                    log(*conn, "server dropped connection");
                    close(conn);
                    return;
                }
            }

            if (state.is({fd_state::HUP, fd_state::ERROR})) {
                int code;
                socklen_t size = sizeof(code);
                socket_wrap const &sock = *static_cast<socket_wrap const *>(&server);
                sock.get_option(SO_ERROR, &code, &size);
                annotated_exception exception(to_string(sock) + " send", code);
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

            if (state.is(fd_state::IN)) {
                resp->read_from(server);

                if (resp->can_write()) {
                    conn->get_client_registration().update({fd_state::OUT, fd_state::RDHUP});
                }

                if (resp->is_read()) {
                    std::string url = to_url(s_rqst->get_header());
                    if (sholud_cache(resp->get_header())) {
                        save_cached(url, resp->get_cache());
                        log(*conn, "response from " + url + " saved to cache");
                    }

                    send_server_response(conn, std::move(*s_rqst), std::move(*resp));
                }
            }
        });
        conn->get_client_registration().update(
                {fd_state::WAIT, fd_state::RDHUP}, [this, conn, s_rqst, resp](fd_state state) {
            file_descriptor const &fd = conn->get_client();
            set_active(conn);

            if (state.is(fd_state::RDHUP)) {
                log(*conn, "client dropped connection");
                close(conn);
                return;
            }

            if (state.is({fd_state::HUP, fd_state::ERROR})) {
                int code;
                socklen_t size = sizeof(code);
                socket_wrap const &sock = *static_cast<socket_wrap const *>(&fd);
                sock.get_option(SO_ERROR, &code, &size);
                annotated_exception exception(to_string(sock) + " send", code);
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

            if (state.is(fd_state::OUT) && resp->can_write()) {
                resp->write_to(fd);

                if (!resp->can_write()) {
                    conn->get_client_registration().update({fd_state::WAIT, fd_state::RDHUP});
                }
            }
        });
    });
}


proxy_server::sockets_t::iterator proxy_server::save_registration(epoll_registration registration, size_t timeout) {
    int fd = registration.get_fd().get();
    return sockets.insert(std::make_pair(fd,
                                         safe_registration(std::move(registration), timeout, ticks))).first;
}

void proxy_server::change_timeout(sockets_t::iterator iterator,
                                  size_t socket_timeout) {
    iterator->second.timeout = socket_timeout;
}

void proxy_server::set_active(sockets_t::iterator iterator) {
    iterator->second.expires_in = ticks + iterator->second.timeout;
}

void proxy_server::close(sockets_t::iterator socket) {
    sockets.erase(socket);
}


proxy_server::connections_t::iterator proxy_server::save_connection(connection conn) {
    return connections.insert(connections.end(), std::move(conn));
}

void proxy_server::set_active(connections_t::iterator iterator) {
    iterator->expires_in = ticks + iterator->timeout;
}

void proxy_server::change_timeout(connections_t::iterator iterator, size_t socket_timeout) {
    iterator->timeout = socket_timeout;
}

void proxy_server::close(connections_t::iterator connection) {
    connections.erase(connection);
}


std::string proxy_server::to_url(request_header const &request) const {
    std::string url = request.get_property("host");
    url += request.get_request_line().get_url();
    return url;
}

void proxy_server::save_cached(std::string url, cached_message const &response) {
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

client_request proxy_server::make_validate_request(request_header rqst, response_header response) const {
    request_header header(rqst.get_request_line());
    header.set_property("host", rqst.get_property("host"));
    if (response.has_property("etag")) {
        header.set_property("if-none-match", response.get_property("etag"));
    }
    if (response.has_property("last-modified")) {
        header.set_property("if-modified-since", response.get_property("last-modified"));
    }
    header.set_property("connection", rqst.get_property("connection"));
    return client_request(header, "");
}

proxy_server::connection::connection() : timeout(0), expires_in(0) { }

proxy_server::connection::connection(epoll_registration client, epoll_registration server, size_t timeout,
                                     size_t ticks) :
        timeout(timeout), expires_in(ticks + timeout), client(std::move(client)), server(std::move(server)) { }

socket_wrap const &proxy_server::connection::get_client() const {
    return *static_cast<socket_wrap const *>(&client.get_fd());
}

socket_wrap const &proxy_server::connection::get_server() const {
    return *static_cast<socket_wrap const *>(&server.get_fd());
}

epoll_registration &proxy_server::connection::get_client_registration() {
    return client;
}

epoll_registration &proxy_server::connection::get_server_registration() {
    return server;
}

void swap(proxy_server::connection &first, proxy_server::connection &second) {
    using std::swap;
    swap(first.client, second.client);
    swap(first.server, second.server);
    swap(first.timeout, second.timeout);
    swap(first.expires_in, second.expires_in);
}

std::string to_string(proxy_server::connection const &conn) {
    using std::to_string;
    std::string res = "connection " +
                      to_string(conn.get_client().get()) +
                      " <-> " +
                      to_string(conn.get_server().get());
    return res;
}

proxy_server::safe_registration::safe_registration() : epoll_registration(), timeout(0), expires_in(0) {
}

proxy_server::safe_registration::safe_registration(epoll_registration registration, size_t timeout, size_t ticks) :
        epoll_registration(std::move(registration)), timeout(timeout), expires_in(ticks + timeout) {
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
    swap(*static_cast<epoll_registration *>(&first), *static_cast<epoll_registration *>(&second));
    swap(first.timeout, second.timeout);
    swap(first.expires_in, second.expires_in);
}

