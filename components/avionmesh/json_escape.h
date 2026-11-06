#pragma once

#include <cstdio>
#include <string>

namespace avionmesh {

// Append s to out as a quoted JSON string, escaping characters that would
// otherwise break the payload. Device and group names are user-supplied
// (claim/rename), so every payload that interpolates one must use this.
inline void append_json_string(std::string &out, const std::string &s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

}  // namespace avionmesh
