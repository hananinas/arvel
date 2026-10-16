#pragma once

#include "agent_runtime.hpp"
#include "buddy_ui.h"
#include "cJSON.h"
#include <string>

/* Native Agent tools: status, battery, display, sound, profile and Wi-Fi scan.
 * Tools run on the chat worker task during a reply. */

// Validates, saves and applies a settings patch ({buddy:{...}, preferences:{...}})
// exactly like the native settings panel; error explains a rejection.
using DeviceSettingsApplier = bool (*)(const cJSON *patch, std::string &error,
                                      const unravel::ToolCallContext &call);

bool device_tools_register(unravel::Agent &agent, DeviceSettingsApplier apply_settings);

// Icon, verb and on-screen target for a device tool call; false for other tools.
// Never includes secrets such as Wi-Fi passwords.
bool device_tool_describe(const char *name, const cJSON *arguments, buddy_ui_tool_kind_t &kind,
                          std::string &verb, std::string &target);

