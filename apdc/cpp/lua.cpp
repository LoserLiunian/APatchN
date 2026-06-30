#include "lua.hpp"

#include "logging.hpp"
#include "module.hpp"
#include "result.hpp"
#include "utils.hpp"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace apd::lua {
namespace {

constexpr const char *CONFIG_DIR = "/data/adb/config";

std::string read_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) bailf("open failed: {}", path);
    std::stringstream ss;
    ss << in.rdbuf();
    if (in.bad()) bailf("read failed: {}", path);
    return ss.str();
}

void save_text(const std::string &filename, const std::string &content) {
    try {
        utils::ensure_dir_exists(CONFIG_DIR);
    } catch (...) {
    }
    std::ofstream out(std::string(CONFIG_DIR) + "/" + filename, std::ios::binary);
    if (!out) bailf("save failed: {}", filename);
    out << content;
}

// --- lua_CFunctions (Lua built as C++, so throwing -> luaL_error is safe) ---

int l_info(lua_State *L) {
    log::info("[Lua] {}", luaL_checkstring(L, 1));
    return 0;
}

int l_warn(lua_State *L) {
    log::warn("[Lua] {}", luaL_checkstring(L, 1));
    return 0;
}

int l_install_module(lua_State *L) {
    const char *zip = luaL_checkstring(L, 1);
    try {
        module::install_module(zip);
    } catch (const std::exception &e) {
        return luaL_error(L, "install_module failed: %s", e.what());
    }
    return 0;
}

int l_set_config(lua_State *L) {
    const char *filename = luaL_checkstring(L, 1);
    const char *content = luaL_checkstring(L, 2);
    try {
        save_text(filename, content);
    } catch (const std::exception &e) {
        return luaL_error(L, "save filed: %s", e.what());
    }
    return 0;
}

int l_get_config(lua_State *L) {
    const char *filename = luaL_checkstring(L, 1);
    std::string path = std::string(CONFIG_DIR) + "/" + filename;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        // Mirror Rust: missing file -> "" (not an error). Module loads with no
        // config and may overwrite/reset state — this is the existing contract.
        lua_pushlstring(L, "", 0);
        return 1;
    }
    try {
        std::string content = read_file(path);
        lua_pushlstring(L, content.data(), content.size());
        return 1;
    } catch (const std::exception &e) {
        // A genuine read failure (chmod 0000, EIO) is NOT the same as missing
        // file; surface it instead of silently returning "" so callers don't
        // proceed on a stale empty config and overwrite the real one.
        return luaL_error(L, "getConfig failed: %s", e.what());
    }
}

void load_all_lua_modules(lua_State *L) {
    // ensure a global `modules` table
    lua_getglobal(L, "modules");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_setglobal(L, "modules");
    } else {
        lua_pop(L, 1);
    }

    const char *modules_dir = "/data/adb/modules";
    std::error_code ec;
    if (!fs::exists(modules_dir, ec)) return;

    for (const auto &entry : fs::directory_iterator(modules_dir, ec)) {
        if (!entry.is_directory(ec)) continue;
        std::string id = entry.path().filename().string();

        // prepend "<dir>/?.so;" to package.cpath
        lua_getglobal(L, "package");
        lua_getfield(L, -1, "cpath");
        const char *old = lua_tostring(L, -1);
        std::string new_cpath = entry.path().string() + "/?.so;" + (old ? old : "");
        lua_pop(L, 1);
        lua_pushstring(L, new_cpath.c_str());
        lua_setfield(L, -2, "cpath");
        lua_pop(L, 1);

        fs::path lua_file = entry.path() / (id + ".lua");
        if (!fs::exists(lua_file, ec)) continue;

        std::string code = read_file(lua_file.string());
        std::string name = lua_file.string();
        if (luaL_loadbuffer(L, code.data(), code.size(), name.c_str()) != LUA_OK) {
            log::error("Failed to eval Lua {}: {}", name, lua_tostring(L, -1));
            lua_pop(L, 1);
            continue;
        }
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            log::error("Failed to eval Lua {}: {}", name, lua_tostring(L, -1));
            lua_pop(L, 1);
            continue;
        }
        if (!lua_istable(L, -1)) {
            log::error("Lua {} did not return a table", name);
            lua_pop(L, 1);
            continue;
        }
        // modules[id] = <result>
        lua_getglobal(L, "modules");
        lua_pushvalue(L, -2);
        lua_setfield(L, -2, id.c_str());
        lua_pop(L, 2); // pop modules + result
    }
}

} // namespace

void run_lua(const std::string &id, const std::string &function, bool on_each_module, bool wait) {
    (void)wait;
    lua_State *L = luaL_newstate();
    if (!L) bail("luaL_newstate failed");
    struct Guard {
        lua_State *L;
        ~Guard() { lua_close(L); }
    } guard{L};

    luaL_openlibs(L);

    lua_pushcfunction(L, l_install_module);
    lua_setglobal(L, "install_module");
    lua_pushcfunction(L, l_info);
    lua_setglobal(L, "info");
    lua_pushcfunction(L, l_warn);
    lua_setglobal(L, "warn");
    lua_pushcfunction(L, l_set_config);
    lua_setglobal(L, "setConfig");
    lua_pushcfunction(L, l_get_config);
    lua_setglobal(L, "getConfig");

    load_all_lua_modules(L);

    lua_getglobal(L, "modules"); // [modules]
    ensure(lua_istable(L, -1), "modules table missing");

    if (on_each_module) {
        lua_pushnil(L); // [modules, nil]
        while (lua_next(L, -2) != 0) {
            // [modules, key, value]
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, function.c_str()); // [.., value, field]
                if (lua_isfunction(L, -1)) {
                    lua_pushstring(L, id.c_str());
                    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                        std::string err = lua_tostring(L, -1);
                        bail(err);
                    }
                } else {
                    lua_pop(L, 1); // pop non-function field
                }
            }
            lua_pop(L, 1); // pop value, keep key
        }
        lua_pop(L, 1); // pop modules
    } else {
        lua_getfield(L, -1, id.c_str()); // [modules, module]
        ensuref(lua_istable(L, -1), "module '{}' not found", id);
        lua_getfield(L, -1, function.c_str()); // [modules, module, func]
        ensuref(lua_isfunction(L, -1), "function '{}' not found in module '{}'", function, id);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            std::string err = lua_tostring(L, -1);
            bail(err);
        }
        lua_pop(L, 2);
    }
}

void exec_stage_lua(const std::string &stage, bool wait, const std::string &superkey) {
    std::string stage_safe = stage;
    for (char &c : stage_safe)
        if (c == '-') c = '_';
    run_lua(superkey, stage_safe, true, wait);
}

} // namespace apd::lua
