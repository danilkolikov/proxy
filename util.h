/*
 * util.h
 *
 *  Created on: Nov 28, 2015
 *      Author: dark_tim
 */

#ifndef UTIL_H_
#define UTIL_H_

#include <iostream>
#include <string>
#include <string.h>
#include <algorithm>
#include <exception>

class annotated_exception : public std::exception {
public:
    annotated_exception() noexcept;
    annotated_exception(annotated_exception const& other) noexcept;
    annotated_exception(std::string tag, std::string message);
    annotated_exception(std::string tag, int errnum);
    virtual ~annotated_exception() noexcept = default;

    virtual const char *what() const noexcept override;
    int get_errno() const;
private:
    static const size_t BUFFER_LENGTH = 64;
    std::string message;
    int errnum;
};

inline std::string to_string(std::string message) {
    return message;
}

inline std::string to_string(char* message) {
    return std::string(message);
}

template <typename S, typename T>
void log(S tag, T message) {
    using ::to_string;
    using std::to_string;
    std::cout << to_string(tag) << ": " << to_string(message) << '\n';
}

void log(annotated_exception const& e);

std::string to_lower(std::string str);

#endif /* UTIL_H_ */
