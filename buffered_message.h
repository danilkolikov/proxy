/*
 * buffered_message.h
 *
 *  Created on: Dec 19, 2015
 *      Author: dark_tim
 */

#ifndef BUFFERED_MESSAGE_H_
#define BUFFERED_MESSAGE_H_

#include <string>
#include <algorithm>

#include "wraps.h"
#include "header_parser.h"

// Message with unlimited size and without HTTP header
struct raw_message {
    raw_message();
    raw_message(raw_message const& other);
    raw_message(raw_message&& other);

    raw_message &operator=(raw_message other);

    bool can_read() const;
    bool can_write() const;

    void read_from(file_descriptor const& fd);
    void write_to(file_descriptor const& fd);

    friend void swap(raw_message& first, raw_message& second);
private:
    static const size_t BUFFER_LENGTH = 8 * 1024;

    size_t read_length, write_length;
    char buffer[BUFFER_LENGTH];
};

using cached_message = std::vector<std::string>;

// Message with HTTP header and fixed size
template<typename T>
struct buffered_message {
    using cache_t = std::vector<std::string>;

    static const size_t INF = (size_t) 1 << (sizeof(size_t) * 4);
    static const size_t BUFFER_LENGTH = 8 * 1024;   // Maximal length of request's header supported by web-browsers

    buffered_message();
    buffered_message(cached_message cache);
    buffered_message(T const &header, std::string const &body);
    buffered_message(buffered_message const &other);
    buffered_message(buffered_message &&other);
    buffered_message &operator=(buffered_message other);

    ~buffered_message() = default;

    bool is_header_read() const;
    bool can_read() const;
    bool can_write() const;

    bool is_read() const;
    bool is_written() const;

    void read_from(file_descriptor const &socket);
    void write_to(file_descriptor const &socket);

    cached_message get_cache() const;
    T get_header() const;

    template<typename S>
    friend void swap(buffered_message<S> &first, buffered_message<S> &second);
private:
    size_t header_length, body_length, read;
    size_t read_length, write_length;
    T header;
    char buffer[BUFFER_LENGTH];

    size_t cur_part;
    std::vector<std::string> cache;
};

using client_request = buffered_message<request_header>;
using server_response = buffered_message<response_header>;

template<typename T>
buffered_message<T>::buffered_message() :
        header_length(0), body_length(INF), read(0), read_length(0), write_length(0),
        header(T()), cur_part(0), cache{} {
}

template<typename T>
buffered_message<T>::buffered_message(cached_message cache)
        : buffered_message() {
    header = T(cache[0]); // Header saved in 0th part
    this->cache = cache;

    header_length = 0; // Header isn't important now

    body_length = 0;
    for (auto it = this->cache.begin(); it != this->cache.end(); it++) {
        body_length += it->size();
    }

    read = body_length;

    read_length = 0;

    cur_part = 0;
}

template<typename T>
buffered_message<T>::buffered_message(T const &header, std::string const &body) : header(header),
                                                                                  cur_part(0),
                                                                                  cache{} {
    std::string message = to_string(header);
    header_length = message.length();
    body_length = body.length();

    message += body;

    read = body_length;

    read_length = header_length + body_length;

    write_length = 0;
    cache.push_back(message);
    cur_part = 0;
}

template<typename T>
buffered_message<T>::buffered_message(buffered_message<T> const &other) :
        header_length(other.header_length), body_length(other.body_length), read(other.read),
        read_length(other.read_length), write_length(other.write_length), header(other.header),
        cur_part(other.cur_part), cache(other.cache) {
}

template<typename T>
buffered_message<T>::buffered_message(buffered_message<T> &&other) {
    swap(*this, other);
}

template<typename T>
buffered_message<T> &buffered_message<T>::operator=(
        buffered_message<T> other) {
    swap(*this, other);
    return *this;
}

template<typename T>
void swap(buffered_message<T> &first, buffered_message<T> &second) {
    using std::swap;
    swap(first.header_length, second.header_length);
    swap(first.body_length, second.body_length);
    swap(first.read, second.read);
    swap(first.read_length, second.read_length);
    swap(first.write_length, second.write_length);
    swap(first.header, second.header);

    swap(first.cur_part, second.cur_part);
    first.cache.swap(second.cache);
}

template<typename T>
bool buffered_message<T>::can_read() const {
    return !is_read();
}

template<typename T>
bool buffered_message<T>::can_write() const {
    return cur_part != cache.size() && write_length < cache[cur_part].length();
}

template<typename T>
bool buffered_message<T>::is_read() const {
    return read >= body_length;
}

template<typename T>
bool buffered_message<T>::is_written() const {
    return is_read() && cur_part == cache.size();
}

template<typename T>
bool buffered_message<T>::is_header_read() const {
    return header_length != 0;
}

template<typename T>
T buffered_message<T>::get_header() const {
    return header;
}


template<typename T>
void buffered_message<T>::read_from(file_descriptor const &socket) {
    size_t should_read =
            (body_length - read > BUFFER_LENGTH - read_length) ? BUFFER_LENGTH - read_length : body_length - read;

    long read_length_cur = socket.read(buffer + read_length, should_read);

    read_length += read_length_cur;
    std::string message(buffer, read_length);
    if (header_length == 0) {
        size_t pos = message.find("\r\n\r\n");
        if (pos == std::string::npos)
            pos = message.find("\n\n");

        if (pos != std::string::npos) {
            pos += 4; // Skip \r\n\r\n
            std::string body = message.substr(pos);
            size_t read_body_length = body.length();
            header = T(message);

            // Modify header

            // And save

            message = to_string(header);
            header_length = message.length();

            message += body;

            if (header.has_property("content-length")) {
                body_length = header.get_int("content-length");
            } else {
                if (header.get_property("transfer-encoding").compare("chunked") == 0) {
                    body_length = INF;
                } else {
                    // ???
                    body_length = 0;
                }
            }

            read = read_body_length;

            cache.push_back(message);
            read_length = 0;
            cur_part = 0;
        }
    } else {
        read += read_length_cur;
        cache.push_back(message);
        read_length = 0;
    }

    if (body_length == INF) {
        if (message.length() >= 5 && message.substr(message.length() - 5).compare("0\r\n\r\n") == 0) {
            body_length = read;
        }
    }
}

template<typename T>
void buffered_message<T>::write_to(file_descriptor const &socket) {
    long write_length_cur = socket.write(cache[cur_part].c_str() + write_length,
                                         cache[cur_part].length() - write_length);
    write_length += write_length_cur;
    // Next part of cache
    if (write_length == cache[cur_part].length()) {
        write_length = 0;
        cur_part++;
    }
}

template<typename T>
cached_message buffered_message<T>::get_cache() const {
    return cache;
}

#endif /* BUFFERED_MESSAGE_H_ */


