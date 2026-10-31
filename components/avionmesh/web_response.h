#pragma once
#include <string>

namespace avionmesh {

// ESPHome's IDF response wrapper defaults unknown status codes to 500. Apply
// these endpoint-specific codes after constructing the wrapped response.
template <typename Request, typename SetStatus>
auto *begin_json_response(Request *request, int code, const std::string &body,
                          SetStatus set_status) {
    auto *response = request->beginResponse(code, "application/json", body);
    switch (code) {
        case 202:
            set_status("202 Accepted");
            break;
        case 403:
            set_status("403 Forbidden");
            break;
        case 503:
            set_status("503 Service Unavailable");
            break;
    }
    return response;
}

}  // namespace avionmesh
