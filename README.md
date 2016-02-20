# Proxy Server

(Not so) Simple HTTP/HTTPS proxy server

* Written on C++ 11
* Uses the power of lambda-functions
* Handling connections in monadic style
* Caches responses for quicker loading pages

Contains:

* wraps.cpp - wraps for linux file descriptors
* resolver.h - multi-thread resolver for ip addresses
* header_parser - simple parser for HTTP-headers
* proxy_server - proxy server

How to launch:
1. Build with make
2. Launch with command proxy_server <PORT>

