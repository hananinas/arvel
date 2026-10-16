#pragma once
#include "niko_home_control.hpp"
#include <array>

namespace niko_home_control::protocol {
constexpr size_t max_frame = 32768, max_devices = 64, max_locations = 32;
constexpr size_t max_properties = 16;
struct Property {
    std::string name, value, description;
    bool status = false, control = false;
    uint64_t sequence = 0, observed_us = 0;
};
struct Device {
    std::string id, name, type, technology, model, location, location_name;
    bool online = false;
    std::vector<Property> properties;
    bool lighting() const;
};
struct Location { std::string id, name; };
struct Desired { std::string id, status; bool has_brightness = false; int brightness = 0; uint64_t published_us = 0; };
// Production parser/validation/outcome policy, also exercised on the host.
// Unknown definitions/classes are projections only, never inferred controls.
class Inventory {
public:
    std::vector<Device> devices;
    std::vector<Location> locations;
    uint64_t sequence = 0, last_observed_us = 0;
    bool ready = false, connected = false;
    std::string error;
    void disconnect();
    void invalidate(const char *reason);
    bool ingest(const char *topic, const char *json, size_t length, uint64_t now);
    const Device *find(const std::string &id) const;
    bool prepare(const char *args, const std::vector<std::string> &allowlist,
                 const unravel::ToolCallContext &call, Desired &desired,
                 std::string &wire, std::string &failure) const;
    std::string page(uint64_t after, size_t count, const std::string &filter = {},
                     const std::string &location = {}, bool observation_current = true) const;
    std::string get(const char *args, bool observation_current = true) const;
    std::string outcome(const Desired &desired, uint64_t baseline,
                        bool published, bool cancelled, const char *reason,
                        bool observation_current = true) const;
    bool matched(const Desired &desired, uint64_t baseline, bool observation_current = true) const;
};
// Sequential fragment validation; empty first topic is forbidden. A full frame
// remains valid only until reset/start of the next frame.
class Fragments {
public:
    explicit Fragments(char *storage) : storage_(storage) {}
    bool append(const char *topic, size_t topic_length, const char *data,
                size_t length, size_t offset, size_t total);
    void reset();
    bool complete() const { return total_ && used_ == total_; }
    const char *data() const { return storage_; }
    size_t size() const { return used_; }
    const char *topic() const { return topic_.data(); }
private:
    char *storage_;
    std::array<char, 96> topic_{};
    size_t used_ = 0, total_ = 0;
};
bool token_expiry(const std::string &jwt, uint64_t &expiry);
bool config_valid(const Config &config, uint64_t utc_seconds, std::string &error);
} // namespace niko_home_control::protocol
