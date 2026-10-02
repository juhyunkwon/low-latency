// Linux:  g++ -std=c++20 -O3 -march=native -DNDEBUG client.cpp -o client
// Windows: g++ -std=c++20 -O3 -DNDEBUG client.cpp -o client.exe -lws2_32
//          cl /std:c++20 /O2 /DNDEBUG client.cpp ws2_32.lib

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
#include <time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
    constexpr uint16_t PORT = 9000;
    constexpr uint64_t WARMUP = 20000;
    constexpr uint64_t ITERATIONS = 1000000;

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

    void pin_realtime(int core)
    {
#if defined(_WIN32) || defined(_WIN64)
        if (core >= 0)
            SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << core);
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

    uint64_t mono_ns()
    {
#if defined(_WIN32) || defined(_WIN64)
        static const uint64_t freq = [] {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            return static_cast<uint64_t>(f.QuadPart);
        }();
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        const uint64_t q = static_cast<uint64_t>(c.QuadPart);
        return (q / freq) * 1000000000ULL +
               ((q % freq) * 1000000000ULL) / freq;
#else
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
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

    bool recv_all(socket_t fd, void* data, size_t size)
    {
        auto* p = static_cast<uint8_t*>(data);
        while (size > 0)
        {
#if defined(_WIN32) || defined(_WIN64)
            const int n = ::recv(fd, reinterpret_cast<char*>(p), static_cast<int>(size), 0);
            if (n > 0)
            {
                p += n;
                size -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && WSAGetLastError() == WSAEINTR)
                continue;
#else
            const ssize_t n = ::recv(fd, p, size, 0);
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

    bool roundtrip(socket_t fd, Message& tx, Message& rx, uint64_t seq)
    {
        tx.sequence = seq;
        tx.value = seq * 10;
        tx.timestamp_ns = mono_ns();
        if (!send_all(fd, &tx, sizeof(tx)))
            return false;
        if (!recv_all(fd, &rx, sizeof(rx)))
            return false;
        return rx.sequence == tx.sequence && rx.value == tx.value;
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
    const int core = cores > 1 ? cores - 2 : 0;
    pin_realtime(core);

    const socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalid)
    {
        sock_perror("socket");
        return 1;
    }

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

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(PORT);
    if (::inet_pton(AF_INET, "127.0.0.1", &server.sin_addr) != 1)
    {
        std::fprintf(stderr, "invalid address\n");
        sock_close(fd);
        return 1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&server), sizeof(server)) < 0)
    {
        sock_perror("connect");
        sock_close(fd);
        return 1;
    }

    std::printf("connected  core %d / %d\n", core, cores);
    std::fflush(stdout);

    Message tx{};
    Message rx{};

    for (uint64_t i = 0; i < WARMUP; ++i)
    {
        if (!roundtrip(fd, tx, rx, i))
        {
            sock_perror("warmup");
            sock_close(fd);
            return 1;
        }
    }

    std::vector<uint64_t> samples(static_cast<size_t>(ITERATIONS));
    uint64_t total = 0;

    for (uint64_t i = 0; i < ITERATIONS; ++i)
    {
        if (!roundtrip(fd, tx, rx, WARMUP + i))
        {
            std::fprintf(stderr, "echo failed at seq=%" PRIu64 "\n", WARMUP + i);
            sock_perror("roundtrip");
            sock_close(fd);
            return 1;
        }
        const uint64_t rtt = mono_ns() - tx.timestamp_ns;
        samples[static_cast<size_t>(i)] = rtt;
        total += rtt;
    }

    const uint64_t n = ITERATIONS;
    auto begin = samples.begin();
    auto mid = begin + static_cast<std::ptrdiff_t>(n / 2);
    auto p99 = begin + static_cast<std::ptrdiff_t>(n * 99 / 100);
    std::nth_element(begin, mid, samples.end());
    const uint64_t med = *mid;
    std::nth_element(begin, p99, samples.end());
    const uint64_t tail = *p99;
    const uint64_t min_ns = *std::min_element(samples.begin(), samples.end());
    const uint64_t max_ns = *std::max_element(samples.begin(), samples.end());

    std::printf(
        "\nResults (%" PRIu64 ", warmup %" PRIu64 ")\n"
        "-------\n"
        "min     : %" PRIu64 " ns\n"
        "p50     : %" PRIu64 " ns\n"
        "average : %.2f ns\n"
        "p99     : %" PRIu64 " ns\n"
        "max     : %" PRIu64 " ns\n",
        n,
        WARMUP,
        min_ns,
        med,
        static_cast<double>(total) / static_cast<double>(n),
        tail,
        max_ns);

    sock_close(fd);
#if defined(_WIN32) || defined(_WIN64)
    WSACleanup();
#endif
    return 0;
}
