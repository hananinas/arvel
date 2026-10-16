#pragma once

#include "agent_runtime.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace niko_home_control {
struct Config {
    bool enabled = false;
    std::string controller, username, jwt, ca_pem, tls_server_name;
    std::vector<std::string> allowed_lights;
};
// Reconfiguration joins the old actor before replacing its credentials. Call
// from a worker, not an audio callback or the GUI lock. Fixed TLS port: 8884.
bool configure(const Config &config, std::string &error);
bool register_tools(unravel::Agent &agent);
std::string status_json();
std::string inventory_json(uint64_t after = 0, size_t count = 20);
void stop();
} // namespace niko_home_control
