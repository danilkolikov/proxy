/*
 * header_parser.h
 *
 * A simple structures for parsing of HTTP headers
 */

#ifndef HEADER_PARSER_H_
#define HEADER_PARSER_H_

#include <vector>
#include <string>
#include "util.h"

struct header_property {
public:
    std::string name, value;

    header_property();
    explicit header_property(std::string const &property);
    header_property(std::string const &name, std::string const &value);
    header_property(header_property const &other);
    header_property(header_property &&other);

    header_property &operator=(header_property other);

    friend std::string to_string(header_property const &p);
    friend void swap(header_property &first, header_property &second);
};

template<typename Line>
struct http_header {
public:
    http_header();
    explicit http_header(Line line);
    explicit http_header(std::string const &message);
    http_header(http_header<Line> const &other);
    http_header(http_header<Line> &&other);

    http_header<Line> &operator=(http_header<Line> other);

    std::string get_property(std::string name) const;
    int get_int(std::string name) const;
    bool has_property(std::string name) const;
    void set_property(std::string name, std::string value);
    void erase_property(std::string name);

    Line get_request_line() const;
    Line &get_request_line();

    template<typename L>
    friend std::string to_string(http_header<L> const &header);
    template<typename L>
    friend void swap(http_header<L> &first, http_header<L> &second);
private:

    using properties_t = std::vector<header_property>;

    Line request_line;
    properties_t properties;
};

struct request_line {
    enum request_type {
        GET, POST, CONNECT, OPTION
    };

    request_line();
    explicit request_line(std::string const &message);
    request_line(request_line const &other);
    request_line(request_line &&other);
    request_line &operator=(request_line other);

    request_type get_type() const;
    std::string get_url() const;
    void set_url(std::string const &url);
    friend std::string to_string(request_line const &request);

    friend void swap(request_line &first, request_line &second);
private:
    std::string type;
    std::string url;
    std::string http;
};

struct response_line {
    response_line();
    explicit response_line(std::string const &message);
    response_line(response_line const &other);
    response_line(response_line &&other);
    response_line &operator=(response_line other);

    int get_code() const;
    std::string get_description() const;

    friend std::string to_string(response_line const &response);
    friend void swap(response_line &first, response_line &second);
private:
    int code;
    std::string description;
    std::string http;
};

using request_header = http_header<request_line>;
using response_header = http_header<response_line>;

template<typename Line>
http_header<Line>::http_header() : request_line(), properties() {
}

template <typename Line>
http_header<Line>::http_header(Line line) : request_line(line), properties{} {

}

template<typename Line>
http_header<Line>::http_header(std::string const &message) : properties() {
    size_t begin = 0;
    size_t end = message.find('\r', begin);
    std::string header = message.substr(begin, end - begin);

    request_line = Line(header);

    while (end < message.size()) {
        // Skipping \r \n
        begin = end + 2;
        end = message.find('\r', begin);

        // If we found the end of the header
        if (begin == end) {
            break;
        }

        std::string property = message.substr(begin, end - begin);
        properties.push_back(header_property(property));
    }

    // Property Proxy-Connection isn't working on some servers
    if (has_property("proxy-connection")) {
        std::string value = get_property("proxy-connection");
        erase_property("proxy-connection");

        if (!has_property("connection")) {
            set_property("connection", value);
        }
    }
}

template<typename Line>
http_header<Line>::http_header(http_header<Line> const &other) :
        request_line(other.request_line), properties(other.properties) {
}

template<typename Line>
http_header<Line>::http_header(http_header<Line> &&other) : http_header<Line>() {
    swap(*this, other);
}

template<typename Line>
http_header<Line> &http_header<Line>::operator=(http_header<Line> other) {
    swap(*this, other);
    return *this;
}

template<typename Line>
bool http_header<Line>::has_property(std::string name) const {
    for (typename properties_t::const_iterator it = properties.cbegin();
         it != properties.cend(); it++) {
        if (it->name.compare(name) == 0) {
            return true;
        }
    }
    return false;
}

template<typename Line>
std::string http_header<Line>::get_property(std::string name) const {
    for (typename properties_t::const_iterator it = properties.cbegin();
         it != properties.cend(); it++) {
        if (it->name.compare(name) == 0) {
            return it->value;
        }
    }
    return "";
}

template<typename Line>
int http_header<Line>::get_int(std::string name) const {
    std::string value = get_property(name);
    return value.empty() ? 0 : std::stoi(value);
}

template<typename Line>
void http_header<Line>::set_property(std::string name, std::string value) {
    for (auto it = properties.begin();
         it != properties.end(); it++) {
        if (it->name.compare(name) == 0) {
            it->value = value;
            return;
        }
    }
    properties.push_back(header_property(name, value));
}

template<typename Line>
void http_header<Line>::erase_property(std::string name) {
    for (auto it = properties.begin(); it != properties.end(); it++) {
        if (it->name.compare(name) == 0) {
            properties.erase(it);
            return;
        }
    }
}

template<typename Line>
Line http_header<Line>::get_request_line() const {
    return request_line;
}

template<typename Line>
Line &http_header<Line>::get_request_line() {
    return request_line;
}

template<typename Line>
void swap(http_header<Line> &first, http_header<Line> &second) {
    swap(first.request_line, second.request_line);
    first.properties.swap(second.properties);
}

template<typename Line>
std::string to_string(http_header<Line> const &header) {
    std::string result = to_string(header.request_line);

    for (typename http_header<Line>::properties_t::const_iterator it =
            header.properties.cbegin(); it != header.properties.cend(); it++) {
        result.append(to_string(*it));
    }
    result += "\r\n";
    return result;
}

#endif /* HEADER_PARSER_H_ */
