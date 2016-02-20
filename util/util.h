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
#include <map>

// Class of exception that saves errno variable
class annotated_exception : public std::exception {
public:
    annotated_exception() noexcept;
    annotated_exception(annotated_exception const &other) noexcept;
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

// Simple LRU cache with logarithmic assymptotic
template<typename K, typename V, size_t MAX_SIZE>
struct simple_cache {
    simple_cache();
    simple_cache(simple_cache const &other) = default;
    simple_cache(simple_cache &&other) = default;

    simple_cache &operator=(simple_cache const &other) = default;
    simple_cache &operator=(simple_cache &&other) = default;


    void insert(K key, V value);

    bool has(K key) const;
    V &find(K key);
    V const &find(K key) const;

    void erase(K key);
    size_t size() const;

private:
    struct entry;
    using values_t = std::map<K, entry>;

    struct entry {
        V value;
        typename values_t::iterator next;
        typename values_t::iterator prev;

        entry(V value) : value(std::move(value)) { }

        entry(entry const &other) = default;
        entry(entry &&other) = default;

        entry &operator=(entry const &other) = default;
        entry &operator=(entry &&other) = default;
    };

    values_t values;
    typename values_t::iterator first, last;
};

inline std::string to_string(std::string const &message) {
    return message;
}

inline std::string to_string(char *message) {
    return std::string(message);
}

// Function for logging
template<typename S, typename T>
void log(S const &tag, T const &message) {
    using ::to_string;
    using std::to_string;
    std::cout << to_string(tag) << ": " << to_string(message) << '\n';
}

void log(annotated_exception const &e);

// To lower case
std::string to_lower(std::string str);

template<typename K, typename V, size_t MAX_SIZE>
simple_cache<K, V, MAX_SIZE>::simple_cache() : values{}, first(values.begin()), last(values.end()) {
}

template<typename K, typename V, size_t MAX_SIZE>
void simple_cache<K, V, MAX_SIZE>::insert(K key, V value) {
    if (values.size() == MAX_SIZE) {
        auto next = first->second.next;
        values.erase(first);
        first = next;
        next->second.prev = next;
    }
    auto it = values.insert(std::make_pair(std::move(key), entry(std::move(value))));
    if (it.second) {
        auto inserted = it.first;
        if (values.size() == 1) {
            first = inserted;
            inserted->second.prev = inserted;
        } else {
            inserted->second.prev = last;
            last->second.next = inserted;
        }
        last = inserted;
        inserted->second.next = inserted;
    }
}

template<typename K, typename V, size_t MAX_SIZE>
bool simple_cache<K, V, MAX_SIZE>::has(K key) const {
    auto it = values.find(std::move(key));
    return it != values.cend();
}

template<typename K, typename V, size_t MAX_SIZE>
V &simple_cache<K, V, MAX_SIZE>::find(K key) {
    auto it = values.find(std::move(key));
    if (it == values.end()) {
        throw annotated_exception("simple cache", "element not found");
    }
    return it->second.value;
}

template<typename K, typename V, size_t MAX_SIZE>
V const &simple_cache<K, V, MAX_SIZE>::find(K key) const {
    auto it = values.find(std::move(key));
    if (it == values.cend()) {
        throw annotated_exception("simple cache", "element not found");
    }
    return it->second.value;
}

template<typename K, typename V, size_t MAX_SIZE>
void simple_cache<K, V, MAX_SIZE>::erase(K key) {
    auto it = values.find(std::move(key));
    if (it == values.end()) {
        return;
    }
    auto prev = it->second.prev;
    auto next = it->second.next;
    if (it == first) {
        first = next;
    } else {
        prev->second.next = next;
    }
    if (it == last) {
        last = prev;
    } else {
        next->second.prev = prev;
    }
    values.erase(it);
}

template<typename K, typename V, size_t MAX_SIZE>
size_t simple_cache<K, V, MAX_SIZE>::size() const {
    return values.size();
}

#endif /* UTIL_H_ */
