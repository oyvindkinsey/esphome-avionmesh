#include "web_response.h"
#include <cassert>
#include <string>

struct Response {};
struct Request {
    Response response;
    std::string status, content_type, body;
    bool constructed{false};
    Response *beginResponse(int code, const char *type, const std::string &data) {
        status = code == 200 ? "200 OK" : "500 Internal Server Error";
        content_type = type;
        body = data;
        constructed = true;
        return &response;
    }
};

int main() {
    for (const auto code : {200, 202, 403, 500, 503}) {
        Request request;
        unsigned overrides = 0;
        auto *response = avionmesh::begin_json_response(
            &request, code, "{\"error\":\"too_many_event_streams\"}", [&](const char *status) {
                assert(request.constructed);  // Wrapper must not undo the override.
                ++overrides;
                request.status = status;
            });
        assert(response == &request.response);
        assert(request.content_type == "application/json");
        assert(request.body == "{\"error\":\"too_many_event_streams\"}");
        assert(overrides == (code == 202 || code == 403 || code == 503 ? 1u : 0u));
        if (code == 503)
            assert(request.status == "503 Service Unavailable");
        if (code == 202)
            assert(request.status == "202 Accepted");
        if (code == 403)
            assert(request.status == "403 Forbidden");
        if (code == 200)
            assert(request.status == "200 OK");
        if (code == 500)
            assert(request.status == "500 Internal Server Error");
    }
}
