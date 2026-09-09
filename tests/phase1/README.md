# Phase 1 bounded-resource regressions

Standalone CMake project because the root project configures Windows/FFmpeg
components unconditionally. No downloads or production Linux port are added.
Requires a C++23 compiler, CMake, pthreads and the existing nlohmann-json package.

```sh
cmake -S tests/phase1 -B /tmp/monolith-phase1-build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wpedantic -Werror'
cmake --build /tmp/monolith-phase1-build --parallel
ctest --test-dir /tmp/monolith-phase1-build -V
```

For ASan/UBSan, use another build directory and append
`-fsanitize=address,undefined -fno-omit-frame-pointer` to `CMAKE_CXX_FLAGS`.

`replay_packet_ring_test` exercises the production header used by ReplayBuffer,
with the real EncodedPacket ref-counted payload representation (synthetic sizes,
no encode/mux). `ipc_server_test` compiles the real server through tiny Linux
socket/Win32-message adapters; it binds **127.0.0.1:45991**, which must be free.
It takes about 67 seconds because it tests the real 30-second deadlines, not
shortened test constants. The test also checks completed-worker reclamation
only after joining the vector's owner; it does not race-read server internals.

Coverage: no/one/two video keys, oversize GOP/video/audio, exact byte cap, packet
count cap, empty payload, invalid limits, timestamp extremes/skew, residence age,
shared save snapshot lifetime; malformed/NUL/deep JSON, exact/oversized lines,
pipelining/CRLF, idle clients beyond 5 and 30 seconds, first-byte trickle deadline,
250 reconnects, 16-client rejection, worker reclamation without another accept,
short writes, send failure/non-reading-peer deadline/interruption, repeated
stop with idle/partial clients.

Not covered here: MSVC/Winsock behavior, FFmpeg decode/save integration, actual
Windows handle/private-byte soaks, thread/allocation failure injection, slow
application callbacks. Callbacks remain synchronous and stop must wait for them;
these tests establish transport resource bounds, not a universal shutdown SLA.
