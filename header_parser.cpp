#include "header_parser.h"
#include <utility>
#include <algorithm>

// Property in header
header_property::header_property() : name(""), value("") { }

header_property::header_property(std::string const &property) {
    size_t begin = 0;
    size_t end = property.find(':', begin);

    name = to_lower(property.substr(begin, end - begin));

    // Skip ": "

    end++;
    while (end < property.length() && property[end] == ' ') {
        end++;
    }
    begin = end;

    // Rest is the value
    value = property.substr(begin, property.size() - begin);
}

header_property::header_property(std::string const &name,
                                 std::string const &value) :
        name(name), value(value) {
}

header_property::header_property(header_property const &other) :
        name(other.name), value(other.value) {
}

header_property::header_property(header_property &&other) :
        name(std::move(other.name)), value(std::move(other.value)) {
}

header_property &header_property::operator=(header_property other) {
    swap(*this, other);
    return *this;
}

std::string to_string(header_property const &p) {
    return p.name + ": " + p.value + "\r\n";
}

void swap(header_property &first, header_property &second) {
    std::swap(first.name, second.name);
    std::swap(first.value, second.value);
}

// Request from client

request_line::request_line() : type(""), url(""), http("") { }

request_line::request_line(std::string const &line) {
    size_t begin = 0;
    size_t end = line.find(' ', begin);
    type = line.substr(begin, end - begin);

    begin = end + 1;
    end = line.find(' ', begin);

    if (line[begin] != '/') {
//		 Absolute address
        begin = line.find(':', begin); // find "http:"
        begin = line.find('/', begin + 3); // skip "://" and find '/'
    }

    url = line.substr(begin, end - begin);

    begin = end + 1;
    http = line.substr(begin, line.size() - begin);
}

request_line::request_line(request_line const &other) :
        type(other.type), url(other.url), http(other.http) {
}

request_line::request_line(request_line &&other) : request_line() {
    swap(*this, other);
}

request_line &request_line::operator=(request_line other) {
    swap(*this, other);
    return *this;
}

request_line::request_type request_line::get_type() const {
    if (type.compare("GET") == 0) {
        return GET;
    }
    if (type.compare("POST") == 0) {
        return POST;
    }
    return GET;
}

std::string request_line::get_url() const {
    return url;
}

void request_line::set_url(std::string const &url) {
    this->url = url;
}

std::string to_string(request_line const &line) {
    return line.type + " " + line.url + " " + line.http + "\r\n";
}

void swap(request_line &first, request_line &second) {
    std::swap(first.type, second.type);
    std::swap(first.url, second.url);
    std::swap(first.http, second.http);
}

// Response from server

response_line::response_line() : code(-1), description(""), http("") { }

response_line::response_line(std::string const &line) : response_line() {
    size_t begin = 0;
    size_t end = line.find(' ', begin);
    http = line.substr(begin, end - begin);
    // Skip ' '
    begin = end + 1;
    end = line.find(' ', begin);

    code = std::stoi(line.substr(begin, end - begin));

    // Skip ' '
    begin = end + 1;
    description = line.substr(begin, line.size() - begin);
}

response_line::response_line(response_line const &other) :
        code(other.code), description(other.description), http(
        other.http) {
}

response_line::response_line(response_line &&other) {
    swap(*this, other);
}

response_line &response_line::operator=(response_line other) {
    swap(*this, other);
    return *this;
}

int response_line::get_code() const {
    return code;
}

std::string response_line::get_description() const {
    return description;
}

std::string to_string(response_line const &line) {
    return line.http + " " + std::to_string(line.code) + " " + line.description + "\r\n";
}

void swap(response_line &first, response_line &second) {
    std::swap(first.code, second.code);
    std::swap(first.description, second.description);
    std::swap(first.http, second.http);
}
