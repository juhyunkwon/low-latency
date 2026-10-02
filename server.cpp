// Linux:  g++ -std=c++20 -O3 -march=native -DNDEBUG server.cpp -o server
// Windows: g++ -std=c++20 -O3 -DNDEBUG server.cpp -o server.exe -lws2_32
//          cl /std:c++20 /O2 /DNDEBUG server.cpp ws2_32.lib

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
    constexpr uint16_t PORT = 9000;

    struct alignas(64) Message
    {
        uint64_t sequence;
        uint64_t timestamp_ns;
        uint64_t value;
    };

    static_assert(sizeof(Message) == 64);

#if defined(_WIN32) || defined(_WIN64)
    using socket_t = SOCKET;
    constexpr socket_t kInvalid = INVALID_SOCKET;

    void sock_close(socket_t fd) { closesocket(fd); }

    void sock_perror(const char* what)
    {
        std::fprintf(stderr, "%s: %d\n", what, WSAGetLastError());
    }
#else
    using socket_t = int;
    constexpr socket_t kInvalid = -1;

#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46
#endif
#ifndef TCP_NOTSENT_LOWAT
#define TCP_NOTSENT_LOWAT 25
#endif

    void sock_close(socket_t fd) { ::close(fd); }

    void sock_perror(const char* what) { perror(what); }
#endif

    int cpu_count()
    {
#if defined(_WIN32) || defined(_WIN64)
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        return static_cast<int>(info.dwNumberOfProcessors);
#else
        const long n = sysconf(_SC_NPROCESSORS_ONLN);
        return n > 0 ? static_cast<int>(n) : 1;
#endif
    }

    // 서버는 마지막 코어. 클라이언트는 그 옆 코어를 쓰므로 서로 밀리지 않는다.
    void pin_realtime(int core)
    {
#if defined(_WIN32) || defined(_WIN64)
        if (core >= 0)
        {
            const DWORD_PTR mask = DWORD_PTR{1} << core;
            SetThreadAffinityMask(GetCurrentThread(), mask);
        }
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#else
        if (core >= 0)
        {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(core, &set);
            if (sched_setaffinity(0, sizeof(set), &set) != 0)
                sock_perror("sched_setaffinity");
        }
        mlockall(MCL_CURRENT | MCL_FUTURE);
        if (cpu_count() >= 2)
        {
            sched_param sp{};
            sp.sched_priority = 10;
            if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
                sock_perror("sched_setscheduler");
        }
#endif
    }

    bool send_all(socket_t fd, const void* data, size_t size)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        while (size > 0)
        {
#if defined(_WIN32) || defined(_WIN64)
            const int n = ::send(fd, reinterpret_cast<const char*>(p), static_cast<int>(size), 0);
            if (n > 0)
            {
                p += n;
                size -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && WSAGetLastError() == WSAEINTR)
                continue;
#else
            const ssize_t n = ::send(fd, p, size, MSG_NOSIGNAL);
            if (n > 0)
            {
                p += n;
                size -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
#endif
            return false;
        }
        return true;
    }

    void tune(socket_t fd)
    {
        int one = 1;
        int zero = 0;
#if defined(_WIN32) || defined(_WIN64)
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&zero), sizeof(zero));
#else
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &zero, sizeof(zero));
        int busy = 50;
        setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy, sizeof(busy));
        unsigned int lowat = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &lowat, sizeof(lowat));
        int tos = IPTOS_LOWDELAY;
        setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
#endif
    }

    // 연결 하나에서 64바이트가 찰 때마다 그대로 돌려준다.
    // epoll 로 재우지 않고 recv 에 붙어 있는다.
    void echo_session(socket_t fd)
    {
        alignas(64) uint8_t buf[4096];
        size_t len = 0;

        for (;;)
        {
#if defined(_WIN32) || defined(_WIN64)
            const int n = ::recv(
                fd,
                reinterpret_cast<char*>(buf + len),
                static_cast<int>(sizeof(buf) - len),
                0);
#else
            const ssize_t n = ::recv(fd, buf + len, sizeof(buf) - len, 0);
#endif
            if (n == 0)
                return;
            if (n < 0)
            {
#if defined(_WIN32) || defined(_WIN64)
                if (WSAGetLastError() == WSAEINTR)
                    continue;
#else
                if (errno == EINTR)
                    continue;
#endif
                return;
            }

            len += static_cast<size_t>(n);
            size_t off = 0;
            while (len - off >= sizeof(Message))
            {
                if (!send_all(fd, buf + off, sizeof(Message)))
                    return;
                off += sizeof(Message);
            }
            if (off > 0)
            {
                len -= off;
                if (len > 0)
                    std::memmove(buf, buf + off, len);
            }
        }
    }
}

int main()
{
#if defined(_WIN32) || defined(_WIN64)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        sock_perror("WSAStartup");
        return 1;
    }
#endif

    const int cores = cpu_count();
    const int core = cores > 0 ? cores - 1 : 0;
    pin_realtime(core);

    const socket_t listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == kInvalid)
    {
        sock_perror("socket");
        return 1;
    }

    int one = 1;
#if defined(_WIN32) || defined(_WIN64)
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#else
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        sock_perror("bind");
        sock_close(listen_fd);
        return 1;
    }
    if (::listen(listen_fd, 128) < 0)
    {
        sock_perror("listen");
        sock_close(listen_fd);
        return 1;
    }

    std::printf("TCP echo server: port %u  core %d / %d\n", PORT, core, cores);
    std::fflush(stdout);

    for (;;)
    {
        const socket_t fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd == kInvalid)
        {
#if defined(_WIN32) || defined(_WIN64)
            if (WSAGetLastError() == WSAEINTR)
                continue;
#else
            if (errno == EINTR)
                continue;
#endif
            sock_perror("accept");
            break;
        }

        tune(fd);
        std::printf("client connected\n");
        std::fflush(stdout);
        echo_session(fd);
        std::printf("client closed\n");
        std::fflush(stdout);
        sock_close(fd);
    }

    sock_close(listen_fd);
#if defined(_WIN32) || defined(_WIN64)
    WSACleanup();
#endif
    return 0;
}
