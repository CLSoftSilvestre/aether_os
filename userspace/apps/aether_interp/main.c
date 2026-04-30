/*
 * AetherOS — AetherScript Interpreter  (Phase 5.5)
 * File: userspace/apps/aether_interp/main.c
 *
 * Usage:  aether_interp <script_path>
 *
 * Reads a Lua 5.4 script from the VFS, executes it, and prints output to
 * stdout (fd 1).  When launched by AetherEditor, stdout is the write end of
 * a pipe whose read end is read by the editor's output panel.
 *
 * Memory: 2MB static pool for the Lua VM (never freed — scripts are short-
 * lived and the process exits after the script finishes).
 */

#include <sys.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Lua headers (vendor/lua54/aether/luaconf.h is picked up first) ─────── */
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* ── FILE* globals required by luaconf.h shim ───────────────────────────── */
static LuaFile_t _stdout_file = { 1 };
static LuaFile_t _stderr_file = { 2 };
LuaFile_t *_aether_stdout = &_stdout_file;
LuaFile_t *_aether_stderr = &_stderr_file;

/* ── Lua allocator — 2MB static pool ───────────────────────────────────── */
#define LUA_POOL_SIZE (2 * 1024 * 1024)

static unsigned char g_pool[LUA_POOL_SIZE];
static unsigned int  g_pool_top;

static void *lua_alloc_fn(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    if (nsize == 0) return NULL;    /* free — bump allocator ignores */
    if (ptr && nsize <= osize) return ptr;  /* shrink in place */

    unsigned int aligned = (unsigned int)((nsize + 7u) & ~7u);
    if (g_pool_top + aligned > LUA_POOL_SIZE) return NULL;  /* OOM */

    void *p = &g_pool[g_pool_top];
    g_pool_top += aligned;

    if (ptr && osize > 0) {
        size_t copy = osize < nsize ? osize : nsize;
        unsigned char *s = (unsigned char *)ptr;
        unsigned char *d = (unsigned char *)p;
        for (size_t i = 0; i < copy; i++) d[i] = s[i];
    }
    return p;
}

/* ── os library shim ─────────────────────────────────────────────────────── */
/* os.version → version string; os.spawn(path) → launches an app            */

static int l_os_version(lua_State *L)
{
    lua_pushstring(L, "AetherOS 0.5.5");
    return 1;
}

static int l_os_spawn(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    long pid = sys_spawn(path);
    lua_pushinteger(L, (lua_Integer)pid);
    return 1;
}

static int l_os_ticks(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)sys_get_ticks());
    return 1;
}

static int l_os_exit(lua_State *L)
{
    int code = (int)luaL_optinteger(L, 1, 0);
    sys_exit(code);
    return 0;
}

static const luaL_Reg g_os_lib[] = {
    { "version",  NULL },   /* set below as a string field, not a function */
    { "spawn",    l_os_spawn   },
    { "ticks",    l_os_ticks   },
    { "exit",     l_os_exit    },
    { NULL, NULL }
};

static void open_os_lib(lua_State *L)
{
    lua_newtable(L);
    /* Register function entries */
    for (const luaL_Reg *r = g_os_lib; r->name; r++) {
        if (!r->func) continue;
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
    /* os.version is a string field, not a function */
    lua_pushstring(L, "AetherOS 0.5.5");
    lua_setfield(L, -2, "version");
    lua_setglobal(L, "os");
}

/* ── Script loader ──────────────────────────────────────────────────────── */
#define SCRIPT_BUF_SIZE (64 * 1024)  /* 64 KB max script */

static char g_script[SCRIPT_BUF_SIZE];

static int load_script(const char *path)
{
    long vfd = sys_fs_open(path);
    if (vfd < 0) {
        printf("aether_interp: cannot open '%s'\n", path);
        return -1;
    }

    long n = sys_fs_read(vfd, (char *)g_script, SCRIPT_BUF_SIZE - 1);
    sys_fs_close(vfd);

    if (n < 0) {
        printf("aether_interp: read error on '%s'\n", path);
        return -1;
    }

    g_script[n] = '\0';
    return 0;
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: aether_interp <script.as>\n");
        return 1;
    }

    const char *script_path = argv[1];

    if (load_script(script_path) != 0) return 1;

    /* Create Lua state with our custom allocator */
    lua_State *L = lua_newstate(lua_alloc_fn, NULL);
    if (!L) {
        printf("aether_interp: lua_newstate failed (OOM)\n");
        return 1;
    }

    /* Open standard libraries (base, string, math, table, coroutine, utf8) */
    luaL_openlibs(L);

    /* Replace os lib with our minimal shim */
    open_os_lib(L);

    /* Run the script */
    int rc = luaL_dostring(L, g_script);
    if (rc != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        printf("aether_interp: %s\n", err ? err : "(error)");
        lua_pop(L, 1);
    }

    lua_close(L);
    return rc == LUA_OK ? 0 : 1;
}
