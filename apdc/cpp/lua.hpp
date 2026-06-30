#pragma once
// Lua scripting hooks. Mirrors ../apd/src/lua.rs. Backed by vendored Lua 5.4.
#include <string>

namespace apd::lua {

// Run `function` from module <id>'s Lua table, or (on_each_module) from every
// loaded module's table passing `id` as the argument.
void run_lua(const std::string &id, const std::string &function, bool on_each_module, bool wait);

// Run a boot stage hook on each module: run_lua(superkey, stage_with_underscores, ...).
void exec_stage_lua(const std::string &stage, bool wait, const std::string &superkey);

} // namespace apd::lua
