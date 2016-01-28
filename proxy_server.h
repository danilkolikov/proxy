/*
 * little_server.h
 *
 *  Created on: Dec 5, 2015
 *      Author: dark_tim
 */

#ifndef PROXY_SERVER_H_
#define PROXY_SERVER_H_

#include <map>
#include <string>
#include <memory>
#include <map>
#include <list>

#include "wraps.h"
#include "resolver.h"
#include "buffered_message.h"

// Monadic proxy server
struct proxy_server {
    struct resolver_extra {
        int socket;
        std::string host;
    };

    proxy_server() = delete;

    proxy_server(epoll_wrap &epoll, resolver<resolver_extra> &resolver, uint16_t port, int queue_size);

private:
    // Connection between two epoll_registrations (with timeout)
    struct connection {
        connection();
        connection(epoll_registration client, epoll_registration server,
                   size_t timeout, size_t ticks);
        connection(connection &&other) = default;
        connection &operator=(connection &&other) = default;

        socket_wrap const &get_client() const;
        socket_wrap const &get_server() const;

        epoll_registration &get_client_registration();
        epoll_registration &get_server_registration();

        friend void swap(connection &first, connection &second);

        size_t timeout, expires_in;
    private:
        epoll_registration client, server;
    };

    friend void swap(connection &first, connection &second);

    friend std::string to_string(connection const &conn);

    // epoll_registration with timeout
    struct safe_registration : epoll_registration {
        size_t timeout;
        size_t expires_in;

        safe_registration();
        safe_registration(epoll_registration registration, size_t timeout, size_t cur_ticks);
        safe_registration(safe_registration &&other);
        safe_registration &operator=(safe_registration &&other);
    };

    friend void swap(safe_registration &first, safe_registration &second);

    // Types of used containers
    using cache_t = std::map<std::string, cached_message>;
    using sockets_t = std::map<int, safe_registration>;
    using connections_t = std::list<connection>;
    using resolver_t = resolver<resolver_extra>;
    using resolved_ip_t = resolved_ip<resolver_extra>;

    // Actions used in connections handling

    template <typename... Args>
    using action_with = std::function<void(Args...)>;

    using action = action_with<>;
    using action_with_connection = action_with<typename connections_t::iterator>;
    using action_with_response = action_with<server_response>;
    using action_with_request = action_with<client_request>;

    using on_resolve_t = std::map<std::pair<int, std::string>,
            action_with_connection>;

    // Default timeouts
    static const size_t TICK_INTERVAL = 2;
    static const size_t SHORT_SOCKET_TIMEOUT = 60 * 2;
    static const size_t LONG_SOCKET_TIMEOUT = 60 * 10;
    static const size_t INFINITE_TIMEOUT = (size_t) 1 << (4 * sizeof(size_t));

    // Monadic-like functions for handling connections
    // Connect to server and do "next"
    void connect_to_server(sockets_t::iterator sock, std::string host, action_with_connection next);

    // Read message and do "next"
    template <typename T, typename C>
    void read(epoll_registration &from, buffered_message<T> message, C iterator, action_with<buffered_message<T>> next);

    // Send message and do "next"
    template <typename T, typename C>
    void send(epoll_registration &to, buffered_message<T> message, C iterator, action next);

    // Send request, read response and do "next"
    template <typename C>
    void send_and_read(epoll_registration &to, client_request, C iterator, action_with_response next);

    // Read response and send it to client during reading
    void fast_transfer(connections_t::iterator conn, client_request rqst);

    // Send response and save to cache if it's possible
    void send_server_response(connections_t::iterator conn, client_request rqst, server_response);

    // Default actions
    // Connect to host from header
    action_with_request first_request_read(sockets_t::iterator client);
    // If host differs from host, then connect. Otherwise, handle request
    action reuse_connection(connections_t::iterator conn, std::string old_host);
    // Start validation or start transfer
    action_with_connection handle_client_request(client_request rqst);
    // Decide, can we send cached or should download response again
    action_with_response handle_validation_response(connections_t::iterator conn, client_request rqst, server_response cached);
    // Connect to server
    epoll_wrap::handler_t make_server_connect_handler(connections_t::iterator conn, resolved_ip_t ip,
                                                      on_resolve_t::iterator query);
    // Keeping active sockets and connections
    sockets_t::iterator save_registration(epoll_registration registration, size_t socket_timeout);
    void close(sockets_t::iterator socket);
    void change_timeout(sockets_t::iterator, size_t socket_timeout);
    void set_active(sockets_t::iterator iterator);

    connections_t::iterator save_connection(connection conn);
    void change_timeout(connections_t::iterator, size_t socket_timeout);
    void close(connections_t::iterator socket);
    void set_active(connections_t::iterator iterator);

    // Caching
    bool sholud_cache(response_header const &header) const;
    client_request make_validate_request(request_header rqst, response_header response) const;
    std::string to_url(request_header const &request) const;
    void save_cached(std::string url, cached_message const &response);
    bool is_cached(request_header const &request) const;
    cached_message get_cached(request_header const &request) const;
    void delete_cached(request_header const &request);

    epoll_wrap &epoll;
    resolver_t &rt;

    size_t ticks;                   // Current time
    on_resolve_t on_resolve;        // Sockets on resolve. Should be here for not giving wrong IP to client
    connections_t connections;      // Active connections
    sockets_t sockets;              // Active sockets
    cache_t cache;                  // Cache

    sockets_t::iterator listener;
    sockets_t::iterator notifier;
    sockets_t::iterator timer;

};


#endif /* PROXY_SERVER_H_ */
