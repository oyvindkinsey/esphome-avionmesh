# Authenticated controller observer candidate

The pinned avionmesh-cpp v1.1.3 commit is
`0cf84e0b946ace134a401503db3b8ec62e801c2c`; its recsrmesh submodule is
`d97c42f653f0079cd614dec31930475e428bf2e2`. That released public API has
`set_rx_callback` with the two source fields, opcode and payload, but has no
observer carrying authenticated sequence and receipt timestamp.

The included [library patch](patches/recsrmesh-rx-observer.patch) proposes the
additive `set_rx_observer(MeshContext &, RxObserver)` API. It is a local candidate,
not a published version or an existing supported released API. Apply it to a
separate checkout of the exact recsrmesh base above, then build the containing
avionmesh-cpp with that patched submodule. Do not modify ESPHome's cached pinned
library in place. The experimental library commit is
`8d4baaf7fe1da058bf309646693696654259a19b`.

`RECSRMESH_RX_OBSERVER_API >= 1`, defined by the candidate header, selects the
component's observer path. Do not force this macro against v1.1.3: the new types
and function must actually be present. No YAML or released dependency pin changes.
The observer copies authenticated source, sequence, MCP header field, opcode,
payload and library receipt timestamp into the existing bounded queue. The same
40-byte record stores nine bytes of metadata plus at most 23 payload bytes
(the 40-byte MTL bound minus 14 crypto bytes and three MCP header bytes).
Its 64 queue records and 64 duplicate-window records retain their original size.
There is no second decryption or heap allocation in this callback. The library's
native RX, discovery, association and transport handlers are preserved.

**Default v1.1.3 still uses the existing internal MTL wrapper and later decrypts
and parses again. The review concern is not fully resolved in the default build.**
The feature guard preserves that build while the dependency change is reviewed.
Before enabling by default: review and release the recsrmesh API, bump the
avionmesh-cpp submodule/release, pin that reviewed release here, and validate both
library crypto suites and the ESP32 candidate. Do not invent a published pin.

The observer only copies; inventory lookup and product gating still use the
authenticated crypto source on the main loop. The MCP header is not controller
identity. Duplicate keys now use all authenticated metadata plus payload rather
than ciphertext; differences in receipt time do not turn retransmissions into
new events. Existing freshness, reconnect epochs, bounded processing (eight
records per loop), offline discard, and model/programming gates are unchanged.
Neither the new API nor the queue adds an unbounded replay buffer. Crypto
reinitialization (including failure) and factory reset clear the queue under its
mutex: an old authenticated snapshot cannot be checked against the new mesh key.
The drop counter remains cumulative across that explicit queue clear.

Offline fixtures exercise both compile-time paths. The legacy case checks the
original handler is still invoked when the queue fills. The observer case checks
the native transport callback is untouched. Both cover copied metadata and payload
lifetime, authenticated sender distinct from header field, sequence/source-aware
deduplication, malformed/oversized snapshots, queue overflow, and a concurrent
producer/consumer. The library fixture additionally covers native RX ordering,
authentication error returns, MCP parse failures, discovery/association dispatch,
receipt timestamps for split and LOW-timeout frames, wrap and reinitialization.
The library fixture uses crypto/protocol doubles; it proves dispatch contracts,
not cryptographic correctness.

Native commands (run from component repository):

```sh
clang++ -std=c++20 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
  -Itests/stubs -Icomponents/avionmesh tests/test_controller_rx.cpp \
  -o /tmp/controller-rx-legacy-test
/tmp/controller-rx-legacy-test
clang++ -std=c++20 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
  -DAVIONMESH_TEST_RX_OBSERVER=1 -Itests/stubs -Icomponents/avionmesh \
  tests/test_controller_rx.cpp -o /tmp/controller-rx-observer-test
/tmp/controller-rx-observer-test
```

## Deferred batched hardware validation

No physical test, controller action, firmware upload or library publication was
performed for this change. The batched session must compare native light/status,
UUID discovery and claim/association behavior before/after; exercise multiple
active 81/91/126/127 controllers with group and local-report modes; verify native
RX and controller-event source/sequence agree; exercise repeated commands, burst
queue pressure, MQTT/mesh disconnect and reconnect with no delayed publication;
confirm no duplicates/replays or missing ordinary status replies. Include the
LOW-only timeout path and ensure its older receipt time expires correctly.

Record event/overflow counters and actual before/after free/minimum/largest heap
watermarks under the same inventory, board/PSRAM, compiler, library and YAML
settings. An 81-device-class inventory with several active controllers is the
review target. Use the separate memory instrumentation and a minimal-instrumented
upstream baseline; boot-lifetime minima and interval samples are different
measurements. Queue `sizeof` invariance is not an ESP32 heap result.
