#pragma once
#include "agent_runtime.hpp"
#include "buddy_ui.h"
#include "cJSON.h"
#include <string>
bool desk_tools_register(unravel::Agent &agent);
bool desk_tool_describe(const char *name, const cJSON *arguments, buddy_ui_tool_kind_t &kind,
                        std::string &verb, std::string &target);
