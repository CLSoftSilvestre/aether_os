/*
 * AetherOS Lua standard library init — safe subset only.
 * Replaces the standard linit.c; excludes io, os, package, debug
 * which require POSIX headers we don't provide.
 */

/* Global errno variable referenced by extern declaration in luaconf.h */
int _aether_errno;
#define linit_c
#define LUA_CORE

#include "lprefix.h"
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

static const luaL_Reg loadedlibs[] = {
    {LUA_GNAME,       luaopen_base},
    {LUA_COLIBNAME,   luaopen_coroutine},
    {LUA_TABLIBNAME,  luaopen_table},
    {LUA_STRLIBNAME,  luaopen_string},
    {LUA_MATHLIBNAME, luaopen_math},
    {LUA_UTF8LIBNAME, luaopen_utf8},
    {NULL, NULL}
};

LUALIB_API void luaL_openlibs(lua_State *L)
{
    const luaL_Reg *lib;
    for (lib = loadedlibs; lib->func; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }
}
