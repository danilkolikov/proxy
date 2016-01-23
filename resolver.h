#ifndef RESOLVER_H_
#define RESOLVER_H_

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <netdb.h>
#include <string>
#include <utility>
#include <memory>
#include <deque>
#include <utility>
#include <signal.h>
#include <pthread.h>

#include "wraps.h"

#include "util.h"

template<typename T>
struct resolved_ip;

template<typename T>
void swap(resolved_ip<T> &first, resolved_ip<T> &second);

template<typename T>
struct resolver;

template<typename T>
struct resolved_ip {
    using ips_t = std::deque<uint32_t>;
    friend struct resolver<T>;

    resolved_ip();

    resolved_ip(ips_t ips, uint16_t port, T &&extra);

    resolved_ip(resolved_ip<T> const& other);

    resolved_ip(resolved_ip<T> &&other);

    ~resolved_ip() = default;

    resolved_ip<T> &operator=(resolved_ip<T> other);

    size_t get_ip_count() const;

    endpoint get_ip() const;

    void next_ip();

    T const &get_extra() const;

    template<typename S>
    friend void swap(resolved_ip<S> &first, resolved_ip<S> &second);

private:

    ips_t ips;
    uint16_t port;
    T extra;

};


// T - type of extra, passed with site url
template<typename T>
struct resolver {
    friend struct resolved_ip<T>;

    resolver();
    resolver(shared_event_fd notifier);

    resolver(resolver<T>&& other) = delete;
    resolver(resolver const &other) = delete;

    resolver<T>& operator=(resolver<T>&& other) = delete;
    resolver &operator=(resolver const &other) = delete;

    ~resolver();

    void stop();

    void resolve_url(std::string host, T const &extra);

    void resolve_url(std::string host, T &&extra);

    resolved_ip<T> get_ip();

private:
    static const size_t THREAD_COUNT = 5;
    static const size_t CACHE_LENGTH = 500;

    struct thread_wrap {
        thread_wrap() = default;

        thread_wrap(resolver<T> &res, size_t number);
        ~thread_wrap();

    private:
        std::thread thread;
        std::mutex mutex;
    };

    using in_query = std::pair<std::string, T>;
    using ips_t = typename resolved_ip<T>::ips_t;
    using cached_ip = std::pair<std::string, ips_t>;
    using unique_thread = std::unique_ptr<thread_wrap>;

    ips_t find_cached(std::string const &host);

    void cache_ip(std::string const &host, ips_t ips);

    ips_t resolve_ip(std::string host, std::string port);

    shared_event_fd notifier;
    bool volatile should_stop;

    std::deque<cached_ip> cache;

    std::queue<in_query> in_queue;
    std::queue<resolved_ip<T>> out_queue;

    std::mutex in_mutex, out_mutex, cache_mutex;
    std::condition_variable cv;
    unique_thread threads[THREAD_COUNT];
};

template<typename T>
resolved_ip<T>::resolved_ip() :
        ips(), port(0), extra() {
}

template<typename T>
resolved_ip<T>::resolved_ip(ips_t ips, uint16_t port, T &&extra) :
        ips(std::move(ips)), port(port), extra(std::move(extra)) {
}


template<typename T>
void swap(resolved_ip<T> &first, resolved_ip<T> &other) {
    using std::swap;
    swap(first.ips, other.ips);
    swap(first.port, other.port);
    swap(first.extra, other.extra);
}

template<typename T>
resolved_ip<T>::resolved_ip(resolved_ip<T> const& other) : ips(other.ips), extra(other.extra) {
}

template<typename T>
resolved_ip<T>::resolved_ip(resolved_ip<T> &&other) : resolved_ip() {
    swap(*this, other);
}

template<typename T>
resolved_ip<T> &resolved_ip<T>::operator=(resolved_ip<T> other) {
    swap(*this, other);
    return *this;
}

template<typename T>
size_t resolved_ip<T>::get_ip_count() const {
    return ips.size();
}

template<typename T>
endpoint resolved_ip<T>::get_ip() const {
    if (ips.empty()) {
        return endpoint();
    }
    uint32_t ip = *ips.begin();

    return endpoint(ip, port);
}

template<typename T>
void resolved_ip<T>::next_ip() {
    if (!ips.empty()) {
        ips.pop_front();
    }

}

template<typename T>
T const &resolved_ip<T>::get_extra() const {
    return extra;
}

template<typename T>
resolver<T>::thread_wrap::thread_wrap(resolver<T> &res, size_t number) {
    thread = std::thread([this, &res, number](int) mutable {
        while (true) {
            std::unique_lock<std::mutex> lock(mutex);
            if (res.should_stop) {
                break;
            }

            bool queue_is_empty;

            {
                std::lock_guard<std::mutex> lg(res.in_mutex);
                queue_is_empty = res.in_queue.empty();
            }


            if (queue_is_empty) {
                res.cv.wait(lock);
                continue;
            }

            in_query p;
            {
                std::lock_guard<std::mutex> lg(res.in_mutex);
                if (res.in_queue.size() == 0) {
                    continue;
                }
                p = std::move(res.in_queue.front());
                res.in_queue.pop();
            }

            size_t pos = p.first.find(":");
            std::string host, port;
            if (pos == std::string::npos) {
                host = p.first;
                port = "80";
            } else {
                host = p.first.substr(0, pos);
                port = p.first.substr(pos + 1);
            }

            ips_t ips = res.find_cached(host);

            if (ips.empty()) {
                try {
                    ips = res.resolve_ip(host, port);
                } catch (annotated_exception const& e) {
                    log(e);
                    ips = ips_t();
                }
            }
            resolved_ip<T> tmp(ips, htons(std::stoi(port)), std::move(p.second));

            {
                std::lock_guard<std::mutex> lg(res.out_mutex);
                res.out_queue.push(std::move(tmp));
            }
            uint64_t u = 1;
            res.notifier->write(&u, sizeof(uint64_t));
        }
    }, 0);
}

template<typename T>
resolver<T>::thread_wrap::~thread_wrap() {
    if (thread.joinable()) {
        thread.join();
    }
}

template<typename T>
resolver<T>::resolver() : notifier(0), should_stop(false) {
}

template<typename T>
resolver<T>::resolver(shared_event_fd notifier) :
        notifier(notifier), should_stop(false) {
    // Ignoring signals from other threads
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    for (size_t i = 0; i < THREAD_COUNT; i++) {
        threads[i] = unique_thread(new thread_wrap(*this, i));
    }

    // Don't ignore in main thread
    sigemptyset(&set);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

template<typename T>
resolver<T>::~resolver() {
    stop();
}

template<typename T>
void resolver<T>::stop() {
    should_stop = true;
    for (size_t i = 0; i < THREAD_COUNT; i++) {
        cv.notify_one();
    }
}

template<typename T>
void resolver<T>::resolve_url(std::string url, T const &extra) {
    {
        std::lock_guard<std::mutex> lg(in_mutex);
        in_queue.push(resolver::in_query(url, extra));
    }
    cv.notify_one();
}

template<typename T>
void resolver<T>::resolve_url(std::string url, T &&extra) {
    {
        std::lock_guard<std::mutex> lg(in_mutex);
        in_queue.push(resolver::in_query(url, extra));
    }
    cv.notify_one();
}

template<typename T>
resolved_ip<T> resolver<T>::get_ip() {
    resolved_ip<T> res;
    {
        std::lock_guard<std::mutex> lg(out_mutex);
        res = std::move(out_queue.front());
        out_queue.pop();
    }
    return res;
}

template<typename T>
typename resolver<T>::ips_t resolver<T>::find_cached(std::string const &host) {
    std::lock_guard<std::mutex> lg(cache_mutex);
    for (auto it = cache.begin(); it != cache.end(); it++) {
        if (it->first.compare(host) == 0) {
            return it->second;
        }
    }
    return ips_t();
}

template<typename T>
void resolver<T>::cache_ip(std::string const &host, typename resolver<T>::ips_t ip) {
    std::lock_guard<std::mutex> lg(cache_mutex);
    cache.push_front({host, ip});
    if (cache.size() > CACHE_LENGTH) {
        cache.pop_back();
    }
}

template<typename T>
typename resolver<T>::ips_t resolver<T>::resolve_ip(std::string host, std::string port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int code;
    struct addrinfo *addr;
    if ((code = getaddrinfo(host.c_str(), port.c_str(), &hints, &addr)) != 0) {
        throw annotated_exception("getaddrinfo", gai_strerror(code));
    }
    ips_t ips;
    struct addrinfo *cur = addr;
    while (cur != 0) {
        sockaddr_in *ep = reinterpret_cast<sockaddr_in *>(cur->ai_addr);
        ips.push_back(ep->sin_addr.s_addr);
        cur = cur->ai_next;
    }
    freeaddrinfo(addr);
    cache_ip(host, ips);
    return ips;
}

#endif /* RESOLVER_H_ */
