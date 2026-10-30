#pragma once

#include "agent_runtime.hpp"
#include <atomic>
#include <string>

namespace nebius_sandbox {
struct Config {
    std::string image = "tag:python:3.12-slim";
    bool allow_network = false;
};
// Only fixed-origin HTTPS is implemented by the production adapter. The body
// and response are bounded; a transport failure after submission is uncertain.
using Request = bool (*)(const char *method, const char *path, const char *body,
    std::string &response, int &status, const unravel::ToolCallContext &call, void *context);

// Must outlive the Agent registry. Calls are synchronous and serial.
class Tools {
public:
    Tools(Config config, Request request, void *context = nullptr);
    bool register_tools(unravel::Agent &agent);
private:
    Config config_;
    Request request_;
    void *context_;
    static bool run(const char *, std::string &, const unravel::ToolCallContext &, void *);
    static bool operation(const char *, std::string &, const unravel::ToolCallContext &, void *);
};

// No tools are registered unless explicitly enabled in menuconfig. Credentials
// remain in the HTTPS adapter, never in model-visible arguments or guest env.
bool register_tools(unravel::Agent &agent, const std::atomic<bool> &cancelled);
} // namespace nebius_sandbox
