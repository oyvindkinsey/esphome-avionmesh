#include "controller_tables.h"
#include <cassert>
#include <cstring>

int main() {
    uint8_t request[10]{};
    size_t length = 0;
    using namespace avionmesh;
    for (unsigned noun : {3u, 27u, 34u, 48u}) {
        assert(build_controller_probe(40022, noun, true, 0, request, length));
        assert(length == 7 && request[0] == 4 && request[1] == noun);
        assert(build_controller_probe(40022, noun, true, 0, request, length, true));
        assert(length == 10 && request[0] == 4 && request[1] == noun);
        for (unsigned i = 2; i < 10; ++i)
            assert(request[i] == 0);
        for (unsigned slot : {0u, 1u, 20u}) {
            assert(build_controller_probe(40022, noun, false, slot, request, length));
            assert(length == 8 && request[0] == 1 && request[1] == noun && request[5] == slot);
            assert(build_controller_probe(40022, noun, false, slot, request, length, true));
            assert(length == 10 && request[0] == 1 && request[1] == noun && request[5] == slot);
            assert(request[8] == 0 && request[9] == 0);
        }
    }
    assert(!build_controller_probe(1200, 27, true, 0, request, length));
    assert(!build_controller_probe(65536, 27, true, 0, request, length));
    assert(
        !build_controller_probe(40022, 10, true, 0, request, length));  // No level-control access.
    assert(!build_controller_probe(40022, 27, true, 1, request, length));
    assert(!build_controller_probe(40022, 27, false, 21, request, length));
    assert(build_controller_probe(40022, 10, false, 0, request, length, true, 0xa1));
    assert(length == 10 && request[0] == 1 && request[1] == 10 && request[4] == 0xa1);
    assert(!build_controller_probe(40022, 10, false, 1, request, length, true, 0xa1));
    assert(build_controller_probe(40022, 27, true, 0, request, length, true, 0xa2));
    assert(request[0] == 4 && request[1] == 27 && request[4] == 0xa2);
    const uint8_t probe_reply[] = {1, 27, 0, 1, 255, 255};
    assert(controller_probe_reply_matches(40022, 27, false, 40022, 0x73, probe_reply, 6));
    assert(!controller_probe_reply_matches(40022, 27, false, 40012, 0x73, probe_reply, 6));
    assert(!controller_probe_reply_matches(40022, 27, true, 40022, 0x73, probe_reply, 6));
    assert(!controller_probe_reply_matches(40022, 48, false, 40022, 0x73, probe_reply, 6));
    assert(!controller_probe_reply_matches(40022, 27, false, 40022, 0x73, probe_reply, 1));
    for (auto slot : {1u, 2u, 3u, 4u})
        assert(controller_program_slot_supported(slot));
    for (auto slot : {0u, 5u, 65538u})
        assert(!controller_program_slot_supported(slot));
    assert(controller_extra_slot_write_allowed(4, false, "01300004ffff0000", true, false));
    assert(controller_extra_slot_write_allowed(1, false, "01300001ffff0000", true, false));
    assert(!controller_extra_slot_write_allowed(1, false, "0130000104b008ff", true, false));
    assert(!controller_extra_slot_write_allowed(4, false, "01300004ffff0000", false, false));
    assert(!controller_extra_slot_write_allowed(4, true, "0130000404b008ff", false, false));
    assert(controller_extra_slot_write_allowed(4, true, "013000045ffc08ff", false, true));
    assert(build_controller_table_read(40101, 0, request, length) && length == 7);
    uint8_t count[] = {4, 0x30, 0, 0, 0, 0, 0};
    assert(!std::memcmp(request, count, length));
    for (uint8_t slot = 1; slot <= 4; ++slot) {
        assert(build_controller_table_read(40101, slot, request, length) && length == 8);
        assert(request[0] == 1 && request[1] == 0x30 && request[5] == slot);
        uint8_t reply[] = {1, 0x30, 0, slot, 0xff, 0xff, 0, 0};
        assert(controller_table_reply_matches(40101, slot, 40101, 0x73, reply, 8));
        assert(!controller_table_reply_matches(40101, slot, 40100, 0x73, reply, 8));
        assert(!controller_table_reply_matches(40101, slot, 40101, 0x74, reply, 8));
        assert(!controller_table_reply_matches(40101, slot, 40101, 0x73, reply, 7));
        reply[3] = slot + 1;
        assert(!controller_table_reply_matches(40101, slot, 40101, 0x73, reply, 8));
    }
    uint8_t count_reply[] = {4, 0x30, 0, 1, 4};
    assert(controller_table_reply_matches(40101, 0, 40101, 0x73, count_reply, 5));
    assert(!build_controller_table_read(0, 1, request, length));
    assert(!build_controller_table_read(1200, 1, request, length));
    assert(!build_controller_table_read(40101, 5, request, length));
    // Unknown record flags are accepted and preserved, never treated as empty.
    uint8_t unknown[] = {1, 0x30, 0, 2, 0, 1, 0x7f, 1};
    assert(controller_table_reply_matches(40101, 2, 40101, 0x73, unknown, 8));
    assert(build_controller_table_program(40101, 2, 0x38, true, 0, request));
    uint8_t clear[] = {5, 0x30, 0, 0, 0x38, 2, 0, 0, 0, 0};
    assert(!std::memcmp(clear, request, 10));  // Exact captured vendor clear.
    assert(build_controller_table_program(40101, 2, 0x41, false, 1200, request));
    uint8_t insert[] = {2, 0x30, 0, 0, 0x41, 2, 4, 0xb0, 8, 255};
    assert(!std::memcmp(insert, request, 10));
    assert(build_controller_table_program(40101, 2, 0x42, false, 24574, request));
    assert(request[6] == 0x5f && request[7] == 0xfe);
    assert(!build_controller_table_program(40101, 0, 0, false, 1200, request));
    assert(!build_controller_table_program(40101, 2, 0, false, 0, request));
    assert(!build_controller_table_program(40101, 2, 0, false, 24576, request));
    uint8_t ack[] = {5, 0x30, 0x38, 1};
    assert(controller_program_reply_matches(40101, 0x38, true, 40101, 0x73, ack, 4));
    assert(!controller_program_reply_matches(40101, 0x39, true, 40101, 0x73, ack, 4));
    assert(!controller_program_reply_matches(40101, 0x38, false, 40101, 0x73, ack, 4));
    assert(!controller_program_reply_matches(40101, 0x38, true, 40201, 0x73, ack, 4));
    auto raw = std::string("0130000204b008ff");
    assert(controller_snapshot_matches(raw, raw, 1000, 2000));
    assert(!controller_snapshot_matches(raw, "01300002ffff0000", 1000, 2000));
    assert(!controller_snapshot_matches(raw, raw, 1000, 16000));
    assert(!controller_snapshot_matches("", raw, 1000, 2000));
    assert(controller_snapshot_matches(raw, raw, 0xfffffff0u, 0));
    assert(controller_event_target_name(40101, 2) == "HA event target: 40101:2");
    assert(build_controller_table_secondary(40201, 3, 0x94, request));
    uint8_t observed_secondary[] = {2, 0x30, 0, 0, 0x94, 0x23, 0, 0, 0, 0};
    assert(!std::memcmp(request, observed_secondary, 10));
    assert(build_controller_table_secondary(40101, 2, 0x42, request) && request[5] == 0x22);
    assert(!build_controller_table_secondary(40101, 5, 0x42, request));
    assert(build_controller_table_program(40101, 3, 0x52, false, 24573, request));
    uint8_t slot3[] = {2, 0x30, 0, 0, 0x52, 3, 0x5f, 0xfd, 8, 255};
    assert(!std::memcmp(request, slot3, 10));
    assert(build_controller_table_secondary(40101, 3, 0x53, request) && request[5] == 0x23);
}
