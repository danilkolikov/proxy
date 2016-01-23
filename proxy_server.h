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

#include "wraps.h"
#include "resolver.h"
#include "buffered_message.h"

struct proxy_server {
    proxy_server() = delete;

    proxy_server(shared_epoll epoll, uint16_t port, int queue_size);

private:
    struct connection {
        connection(shared_socket client, shared_socket server);

        connection(shared_socket client);

        connection(shared_socket client, shared_socket server,
                   client_request request);

        connection(connection const &other);

        connection &operator=(connection other);

        ~connection() = default;

        bool client_connected() const;

        bool server_connected() const;

        bool established() const;

        socket_wrap const &get_client() const;

        socket_wrap const &get_server() const;

        shared_socket get_shared_client() const;

        shared_socket get_shared_server() const;

        client_request &get_request();

        server_response &get_response();

        void clean_request();

        void clean_response();

        void set_client(shared_socket client);

        void set_server(shared_socket server);

        void set_request(client_request request);

        void set_response(server_response response);

        void reset_server();

        void reset_client();

        friend void swap(connection &first, connection &second);

        friend std::string to_string(connection const &conn);

    private:
        shared_socket client, server;
        client_request request;
        server_response response;
    };

    friend void swap(connection &first, connection &second);

    friend std::string to_string(connection const &conn);


    using shared_connection = std::shared_ptr<connection>;

    struct safe_registration {
        epoll_registration registration;
        size_t timeout;
        size_t expires_in;

        safe_registration();

        safe_registration(epoll_registration registration, size_t timeout, size_t cur_ticks);

        safe_registration(safe_registration &&other);

        safe_registration &operator=(safe_registration &&other);
    };

    friend void swap(safe_registration &first, safe_registration &second);

    using action = std::function<void()>;

    struct resolver_extra {
        shared_connection conn;
        action do_next;

        resolver_extra() = default;

        resolver_extra(shared_connection conn, action do_next) :
                conn(conn), do_next(do_next) { }
    };

    using resolver_t = resolver<resolver_extra>;
    using resolved_ip_t = resolved_ip<resolver_extra>;
    using unique_resolver = std::unique_ptr<resolver_t>;

    using cache_t = std::map<std::string, cached_message>;
    using sockets_t = std::map<int, safe_registration>;

    static const size_t TICK_INTERVAL = 2;
    static const size_t SHORT_SOCKET_TIMEOUT = 60 * 2 / TICK_INTERVAL;
    static const size_t LONG_SOCKET_TIMEOUT = 60 * 10 / TICK_INTERVAL;
    static const size_t INFINITE_TIMEOUT = (size_t) 1 << (4 * sizeof(size_t));

    void connect_to_server(shared_connection conn, action do_next);

    bool registered(file_descriptor const &fd);

    void save_registration(epoll_registration registration, size_t socket_timeout);

    epoll_registration &get_registration(file_descriptor const &socket);

    void change_timeout(file_descriptor const &socket, size_t socket_timeout);

    void set_active(file_descriptor const &socket);

    void close(socket_wrap const &socket);

    void close(connection const &conn);


    bool sholud_cache(response_header const &header) const;

    client_request make_validate_request(request_header rqst, response_header response) const;

    std::string to_url(request_header const &request) const;

    void save_cached(request_header const &request, cached_message const &response);

    bool is_cached(request_header const &request) const;

    cached_message get_cached(request_header const &request) const;

    void delete_cached(request_header const &request);

    epoll_wrap::handler_t make_new_client_handler(shared_connection connection, std::string old_host);

    epoll_wrap::handler_t make_server_connect_handler(resolved_ip_t ip);

    epoll_wrap::handler_t make_server_handler(shared_connection connection);

    epoll_wrap::handler_t make_client_handler(shared_connection connection);

    epoll_wrap::handler_t make_cache_validate_handler(shared_connection old_conn, shared_connection validate);

    action start_data_transfer(shared_connection conn);

    action send_cached(shared_connection conn);

    size_t ticks;
    sockets_t sockets;
    cache_t cache;
    unique_resolver rt;
    shared_epoll epoll;
};


#endif /* PROXY_SERVER_H_ */
