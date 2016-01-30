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
#include <atomic>
#include <functional>
#include "wraps.h"

#include "util.h"

template<typename T>
struct resolved_ip;

template<typename T>
void swap(resolved_ip<T> &first, resolved_ip<T> &second);

template<typename T>
struct resolver;

// Extra passed with ips adress through resolver
template<typename T>
struct resolved_ip {
    using ips_t = std::deque<uint32_t>;
    friend struct resolver<T>;

    resolved_ip();
    resolved_ip(ips_t ips, uint16_t port, T &&extra);
    resolved_ip(resolved_ip<T> const &other);
    resolved_ip(resolved_ip<T> &&other);

    resolved_ip<T> &operator=(resolved_ip<T> other);

    ~resolved_ip() = default;

    bool has_ip() const;
    endpoint get_ip() const;
    void next_ip();

    T &get_extra();
    T const& get_extra() const;

    template<typename S>
    friend void swap(resolved_ip<S> &first, resolved_ip<S> &second);

private:

    ips_t ips;
    uint16_t port;
    T extra;

};

// Safe RAII wrap for threads. Starts in constructor, joins in destructor
struct thread_wrap {
    thread_wrap() = default;

    template<class Fn, class... Args>
    thread_wrap(Fn&& func, Args&&... args) : thread(std::forward<Fn>(func), std::forward<Args>(args)...) { };
    thread_wrap(thread_wrap const &other) = default;
    thread_wrap(thread_wrap &&other) = default;

    thread_wrap &operator=(thread_wrap const &other) = default;
    thread_wrap &operator=(thread_wrap &&other) = default;

    ~thread_wrap() {
        if (thread.joinable()) {
            thread.join();
        }
    }

private:
    std::thread thread;
};

// Multi-thread (4 threads) resolver for ip adresses
template<typename T>
struct resolver {
    friend struct resolved_ip<T>;

    resolver();
    
    resolver(resolver<T> &&other) = delete;
    resolver(resolver const &other) = delete;

    resolver<T> &operator=(resolver<T> &&other) = delete;
    resolver &operator=(resolver const &other) = delete;
    ~resolver();

    void stop();

    void resolve_host(std::string host, file_descriptor const &notifier, T extra);
    resolved_ip<T> get_ip();

private:
    static const size_t THREAD_COUNT = 4;
    static const size_t CACHE_SIZE = 500;

    static void main_loop(std::reference_wrapper<resolver> ref);

    struct in_query {
        std::string host;
        file_descriptor const *notifier;
        T extra;
    };

    using ips_t = typename resolved_ip<T>::ips_t;

    ips_t find_cached(std::string const &host);
    void cache_ip(std::string const &host, ips_t ips);

    ips_t resolve_ip(std::string host, std::string port);

    std::queue<in_query> in_queue;
    std::queue<resolved_ip<T>> out_queue;

    simple_cache<std::string, ips_t, CACHE_SIZE> cache;

    std::atomic_bool should_stop;
    std::mutex in_mutex, out_mutex, cache_mutex;
    std::condition_variable cv;
    thread_wrap threads[THREAD_COUNT];
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
resolved_ip<T>::resolved_ip(resolved_ip<T> const &other) : ips(other.ips), extra(other.extra) {
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
bool resolved_ip<T>::has_ip() const {
    return !ips.empty();
}

template<typename T>
endpoint resolved_ip<T>::get_ip() const {
    if (ips.empty()) {
        return endpoint();
    }
    uint32_t ip = *ips.begin();

    return {ip, port};
}

template<typename T>
void resolved_ip<T>::next_ip() {
    if (!ips.empty()) {
        ips.pop_front();
    }

}

template<typename T>
T &resolved_ip<T>::get_extra() {
    return extra;
}

template<typename T>
T const &resolved_ip<T>::get_extra() const {
    return extra;
}


template<typename T>
resolver<T>::resolver() :  should_stop(false)  {
    // Ignoring signals from other threads
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    for (size_t i = 0; i < THREAD_COUNT; i++) {
        threads[i] = thread_wrap(resolver<T>::main_loop, std::ref(*this));
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
    cv.notify_all();
}

template<typename T>
void resolver<T>::resolve_host(std::string host, file_descriptor const &notifier, T extra) {
    {
        std::lock_guard<std::mutex> lg(in_mutex);
        in_queue.push({host, &notifier, std::move(extra)});
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
    if (cache.has(host)) {
        return cache.find(host);
    }
    return ips_t();
}

template<typename T>
void resolver<T>::cache_ip(std::string const &host, typename resolver<T>::ips_t ip) {
    std::lock_guard<std::mutex> lg(cache_mutex);
    cache.insert(host, ip);
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
        throw annotated_exception("resolver", gai_strerror(code));
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
    log("ip of " + host, "saved to cache");
    return ips;
}

template<typename T>
void resolver<T>::main_loop(std::reference_wrapper<resolver<T>> ref) {
    while (true) {
        if (ref.get().should_stop) {
            break;
        }
        {
            std::unique_lock<std::mutex> lock(ref.get().in_mutex);

            if (ref.get().in_queue.empty()) {
                ref.get().cv.wait(lock);
                continue;
            }
        }

        in_query p{};
        {
            std::lock_guard<std::mutex> lg(ref.get().in_mutex);
            if (ref.get().in_queue.size() == 0) {
                continue;
            }
            p = std::move(ref.get().in_queue.front());
            ref.get().in_queue.pop();
        }

        size_t pos = p.host.find(":");
        std::string host, port;
        if (pos == std::string::npos) {
            host = p.host;
            port = "80";
        } else {
            host = p.host.substr(0, pos);
            port = p.host.substr(pos + 1);
        }

        ips_t ips = ref.get().find_cached(host);

        if (ips.empty()) {
            try {
                ips = ref.get().resolve_ip(host, port);
            } catch (annotated_exception const &e) {
                log(e);
                ips = ips_t();
            }
        } else {
            log("ip for " + p.host, "found in cache");
        }
        resolved_ip<T> tmp(ips, htons(std::stoi(port)), std::move(p.extra));

        {
            std::lock_guard<std::mutex> lg(ref.get().out_mutex);
            ref.get().out_queue.push(std::move(tmp));
        }
        uint64_t u = 1;
        p.notifier->write(&u, sizeof(uint64_t));
    }
}

#endif /* RESOLVER_H_ */
