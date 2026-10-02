# low-latency

로컬호스트 TCP 왕복 시간을 재는 64바이트 에코입니다. 서버가 받은 메시지를 그대로 돌려주고, 클라이언트가 100만 번의 RTT를 집계합니다.

## 파일

| 파일 | 역할 |
| --- | --- |
| `server.cpp` | 포트 9000에서 접속을 받고, 연결이 끊길 때까지 에코 |
| `client.cpp` | `127.0.0.1:9000`으로 메시지를 보내고 RTT를 출력 |

한 번에 클라이언트 하나만 붙입니다. 서버는 그 연결이 끝난 뒤 다음 접속을 받습니다.

## 메시지

```text
struct alignas(64) Message {
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint64_t value;
};
```

멤버는 24바이트이고, 정렬 때문에 전송 크기는 64바이트입니다. `value`는 `sequence * 10`입니다. 서버는 바이트를 해석하지 않고 64바이트 단위로 돌려줍니다. 클라이언트가 시퀀스와 값이 같은지 확인합니다.

## 빌드

Linux:

```bash
g++ -std=c++20 -O3 -march=native -DNDEBUG server.cpp -o server
g++ -std=c++20 -O3 -march=native -DNDEBUG client.cpp -o client
```

Windows (MinGW):

```bash
g++ -std=c++20 -O3 -DNDEBUG server.cpp -o server.exe -lws2_32
g++ -std=c++20 -O3 -DNDEBUG client.cpp -o client.exe -lws2_32
```

Windows (MSVC):

```bat
cl /std:c++20 /O2 /DNDEBUG server.cpp ws2_32.lib
cl /std:c++20 /O2 /DNDEBUG client.cpp ws2_32.lib
```

## 실행

서버를 먼저 띄웁니다.

```bash
./server
./client
```

서버는 마지막 코어, 클라이언트는 그 앞 코어에 붙습니다. 코어가 2개 이상이면 둘 다 `SCHED_FIFO` 우선순위 10을 시도합니다. root가 아니면 우선순위 설정은 실패하고, 측정은 그대로 진행됩니다.

## 결과

클라이언트는 2만 번을 워밍업으로 버린 뒤 100만 번을 잽니다. 시각은 `CLOCK_MONOTONIC`입니다.

```text
min
p50
average
p99
max
```

측정 루프 안에는 출력이 없습니다. 진행 로그가 최대값에 섞이지 않게 하기 위해서입니다.

## 지연을 낮추는 설정

데이터 경로는 `epoll`로 재우지 않습니다. 접속이 붙으면 그 소켓의 `recv`에서 64바이트가 찰 때까지 기다렸다가 바로 `send`합니다.

Linux 소켓에는 아래를 켭니다.

- `TCP_NODELAY` — 64바이트를 모으지 않음
- `TCP_QUICKACK` — 응답을 지연 ACK에 묶지 않음
- `SO_BUSY_POLL` 50µs — 수신 대기를 잠시 폴링
- `TCP_NOTSENT_LOWAT` 1 — 작은 송신을 바로 밀어냄
- `IPTOS_LOWDELAY`
- `mlockall` — 측정 중 페이지 폴트 감소

`SO_BUSY_POLL`이 반영되지 않으면 root에서 한 번 켭니다.

```bash
sysctl -w net.core.busy_poll=50 net.core.busy_read=50
```

이 값은 프로세스 전역입니다. 벤치가 끝나면 예전 값으로 되돌리세요.

## 주의

`SCHED_FIFO`로 띄운 프로세스는 해당 코어의 일반 작업을 밀어냅니다. 측정이 끝나면 서버를 종료하세요.
