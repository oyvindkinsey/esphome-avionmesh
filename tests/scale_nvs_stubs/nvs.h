#pragma once
#include <map>
#include <string>
#include <vector>
#include <cstring>
using nvs_handle_t = unsigned;
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_NVS_NOT_FOUND = 1, ESP_FAIL = 2;
constexpr int NVS_READONLY = 0, NVS_READWRITE = 1;
namespace fake_nvs {
inline std::map<std::string, std::map<std::string, std::vector<unsigned char>>> data;
inline std::map<unsigned, std::string> handles;
inline unsigned next_handle = 1;
inline bool fail_commit = false;
inline std::vector<std::string> opened;
}
inline int nvs_open(const char *name, int, nvs_handle_t *out) {
    fake_nvs::opened.emplace_back(name);
    *out = fake_nvs::next_handle++;
    fake_nvs::handles[*out] = name;
    return ESP_OK;
}
inline void nvs_close(nvs_handle_t h) { fake_nvs::handles.erase(h); }
inline int nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t size) {
    auto *p = static_cast<const unsigned char *>(value);
    fake_nvs::data[fake_nvs::handles.at(h)][key] = {p, p + size};
    return ESP_OK;
}
inline int nvs_set_str(nvs_handle_t h, const char *key, const char *value) {
    return nvs_set_blob(h, key, value, std::strlen(value) + 1);
}
inline int nvs_get_blob(nvs_handle_t h, const char *key, void *value, size_t *size) {
    auto &db = fake_nvs::data[fake_nvs::handles.at(h)];
    auto it = db.find(key);
    if (it == db.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (value) {
        if (*size < it->second.size()) return ESP_FAIL;
        std::memcpy(value, it->second.data(), it->second.size());
    }
    *size = it->second.size();
    return ESP_OK;
}
inline int nvs_get_str(nvs_handle_t h, const char *key, char *value, size_t *size) {
    return nvs_get_blob(h, key, value, size);
}
inline int nvs_erase_key(nvs_handle_t h, const char *key) {
    return fake_nvs::data[fake_nvs::handles.at(h)].erase(key) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
inline int nvs_commit(nvs_handle_t) { return fake_nvs::fail_commit ? ESP_FAIL : ESP_OK; }
