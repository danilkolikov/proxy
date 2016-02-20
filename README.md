# Proxy Server

HTTP/HTTPS proxy server with response caching

* Written on C++ 11
* Uses the power of lambda-functions
* Caches responses for quicker loading pages

Contains:

* wraps.h - wraps for linux file descriptors
* resolver.h - multi-thread resolver for ip addresses
* header_parser.h - simple parser for HTTP-headers
* proxy_server.h - proxy server

How to build and use:

1. Generate Makefile with cmake CMakeLists.txt
2. Build with make
3. Launch with command: proxy_server <PORT> . If no port is mentioned, server starts on port 8080


