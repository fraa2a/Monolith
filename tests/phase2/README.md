# Phase 2A media/resource regressions

Standalone Linux project, like phase 1: root CMake requires Windows/vcpkg.
No dependency downloads. Uses the host FFmpeg CLI and libavformat/libavcodec/
libavutil development packages, nlohmann-json, Python, CMake and pthreads.

```sh
cmake -S tests/phase2 -B /tmp/monolith-phase2-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/monolith-phase2-build --parallel
ctest --test-dir /tmp/monolith-phase2-build --output-on-failure
cmake -S tests/phase2 -B /tmp/monolith-phase2-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build /tmp/monolith-phase2-asan --parallel
ctest --test-dir /tmp/monolith-phase2-asan --output-on-failure
cmake --build /tmp/monolith-phase1-build --parallel
ctest --test-dir /tmp/monolith-phase1-build --output-on-failure
```

The media library compiles the **production** trim, mux, disk and RAM replay
sources. The tiny Windows header adapts only ASCII fixture paths/time/directory
creation; FFmpeg encode/decode/mux and filesystem operations are real Linux
integration, not fake media. GNU link wrapping pauses the real `av_write_frame`
call during a disk save to deterministically exercise retention and cancellation.
It does not replace the mux implementation or production limits. This is not
Windows file locking/replacement or MSVC verification.

`media_test.cpp` generates local fixtures in the build's `fixtures/` directory:

- 16 lossless/reencode combinations: MP4/MKV input/output, 30000/1001 video,
  variable frame intervals derived from 60000/1001, B frames, 90/120 kHz versus
  millisecond mux timebases, two AAC tracks at 48/44.1 kHz with different offsets.
- Real full decode with `-xerror`, no frame-rate coercion in the validation sink.
- Shared-anchor bookmark mapping, invalid/end-past-file intervals, unsupported
  same-codec fallback, output-open failure and explicit cancellation.
- Mixed-timebase concatenation, RAM replay with delayed audio arriving after
  newer video, keyframe-aligned restart after a mid-GOP disk save.
- Save paused inside FFmpeg while 100+ seconds of new packets arrive: old
  snapshot inputs stay pinned but unrelated expired segments are deleted.
  A second save decodes the surviving key-aligned segments.
- Cancellation/clear joins the reader before deleting inputs; failed unlink
  remains accounted and blocks new writes (using an occupied owned pathname,
  so the test works even as root). No recursive deletion of unexpected children.
- Oversized packet rejection and budget JSON integer/range validation, including
  malformed types and uint64 extremes. The configurable budget is 512..65536 MiB,
  default 2048. A separate 60-second, >50 MiB CBR fixture exercises incremental
  same-codec reencode with both audio tracks and full output decode.

`verify_timeline.py` independently checks frame hashes for lossless output,
presentation timestamps/frame counts for fractional/VFR reencode, preservation
of every requested presentation frame (plus necessary future references for
lossless), unchanged audio payload hashes/codec/layout/offsets, and actual
container duration matching TrimResult. CTest fixtures ensure generation runs
before these checks.

Not claimed: sample-exact audio cuts, exact lossless end cuts (reordered reference
frames can extend the end), Windows installation/runtime, actual database/file
replacement transactions, real full-budget 512 MiB saturation or 4096-segment
saturation, hardware HEVC/AV1 fallback validation, allocation/thread-failure
injection, or process-memory/performance improvements. FFmpeg codec/mux metadata,
allocator memory and the save output are extra to the encoded-payload budgets.

Continuous-clock regression: `media_test.cpp` additionally encodes one 21-second
fractional/VFR B-frame input with two offset AAC tracks, then saves real disk
segments. It compares every saved audio payload with consecutive source packets,
checks strictly increasing DTS, <=2ms packet-interval/intertrack-offset rounding,
and bounded outer tail omission. This caught actual internal-boundary audio loss
from reapplying the video PTS anchor at every DTS-based segment boundary. A
mid-GOP save is decoded; after clearing retained history, the next-key-started
output receives the same continuity checks (the deliberate key-wait gap is not
claimed continuous). The older repeated pressure fixtures intentionally reuse
AAC priming/tail packets across clock resets and are not continuity evidence.
