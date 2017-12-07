/* trusted.c */

#include "pllua.h"

/*
 * Trusted versions or wrappers for functionality that we need to restrict in a
 * trusted interpreter.
 *
 */

/*
 * this defines the trusted subset of the "os" package (installed as
 * "trusted.os" in the outer environment)
 */

static struct luaL_Reg trusted_os_funcs[] = {
	{ "date", NULL },
	{ "clock", NULL },
	{ "time", NULL },
	{ "difftime", NULL },
    { NULL, NULL }
};

static int pllua_open_trusted_os(lua_State *L)
{
	const luaL_Reg *p;
	lua_getglobal(L, "os");
	luaL_newlib(L, trusted_os_funcs);
	for (p = trusted_os_funcs; p->name; ++p)
	{
		lua_getfield(L, -2, p->name);
		lua_setfield(L, -2, p->name);
	}
	return 1;
}

/*
 * load(chunk[,chunkname[,mode[,env]]])
 *
 * Wrapper must force "mode" to be "t" to disallow loading binary chunks.  Also
 * must force "env" to be the sandbox env if not supplied by the caller.
 *
 * Punts to _G.load after munging the args.
 */
static int pllua_t_load(lua_State *L)
{
	int nargs = lua_gettop(L);
	if (nargs < 4)
	{
		lua_settop(L, 3);
		nargs = 4;
		lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX);
	}
	lua_pushstring(L, "t");
	lua_replace(L, 3);
	lua_getglobal(L, "load");
	lua_insert(L, 1);
	lua_call(L, nargs, LUA_MULTRET);
	return lua_gettop(L);
}

/*
 * user-facing "require" function
 */

static void pllua_t_require_findloader(lua_State *L, int nd, const char *name);

static int pllua_t_require(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	lua_settop(L, 1);
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX_LOADED);
	lua_getfield(L, 2, name);  /* LOADED[name] */
	if (lua_toboolean(L, -1))  /* is it there? */
		return 1;  /* package is already loaded */
	lua_pop(L, 1);  /* remove 'getfield' result */

	if (lua_getfield(L, lua_upvalueindex(1), "searchers") != LUA_TTABLE)
		luaL_error(L, "'package.searchers' must be a table");
	pllua_t_require_findloader(L, -1, name);

	lua_pushstring(L, name);  /* pass name as argument to module loader */
	lua_insert(L, -2);  /* name is 1st argument (before search data) */
	lua_call(L, 2, 1);  /* run loader to load module */
	if (lua_isnil(L, -1))  /* non-nil return? push true if not */
		lua_pushboolean(L, 1);
	lua_pushvalue(L, -1);
	lua_setfield(L, 2, name);  /* LOADED[name] = returned value */
	return 1;
}

/*
 * "require" function helper
 */
static void pllua_t_require_findloader(lua_State *L, int nd, const char *name)
{
	int i;
	luaL_Buffer msg;  /* to build error message */

	nd = lua_absindex(L, nd);

	luaL_buffinit(L, &msg);

	/* iterate over available searchers to find a loader */
	for (i = 1; ; ++i)
	{
		if (lua_rawgeti(L, nd, i) == LUA_TNIL)
		{
			lua_pop(L, 1);  /* remove nil */
			luaL_pushresult(&msg);  /* create error message */
			luaL_error(L, "module '%s' not found:%s", name, lua_tostring(L, -1));
		}

		lua_pushstring(L, name);
		lua_call(L, 1, 2);  /* call it */

		if (lua_isfunction(L, -2))  /* did it find a loader? */
			return;  /* module loader found */
		else if (lua_isstring(L, -2))   /* searcher returned error message? */
		{
			lua_pop(L, 1);  /* remove extra return */
			luaL_addvalue(&msg);  /* concatenate error message */
		}
		else
			lua_pop(L, 2);  /* remove both returns */
	}
}

/*
 * searcher functions are called as
 *
 * searcher(name)  returns func,arg
 *
 */
static int pllua_package_preload_search(lua_State *L)
{
	/* preload searcher works entirely inside the sandbox */
	const char *name = luaL_checkstring(L, 1);
	lua_getfield(L, lua_upvalueindex(1), "preload");
	lua_pushstring(L, name);
	if (lua_gettable(L, -2) == LUA_TNIL)
	{
		lua_pushfstring(L, "\n\tno field package.preload['%s']", name);
		return 1;
	}
	lua_pushnil(L);
	return 2;
}

static int pllua_package_allowed_search(lua_State *L)
{
	/*
	 * allowed searcher works outside the sandbox; the sandbox can't see its
	 * own "allow" list
	 */
	const char *name = luaL_checkstring(L, 1);
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX_ALLOW);
	lua_pushstring(L, name);
	if (lua_gettable(L, -2) == LUA_TNIL)
	{
		lua_pushfstring(L, "\n\tno module '%s' in list of allowed modules", name);
		return 1;
	}
	lua_pushnil(L);
	return 2;
}

static int pllua_open_trusted_package(lua_State *L)
{
	lua_newtable(L);

	lua_pushvalue(L, -1);
	lua_pushcclosure(L, pllua_t_require, 1);
	lua_setfield(L, -2, "require");

	lua_newtable(L);
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX_LOADED);
	lua_setfield(L, -2, "loaded");

	lua_newtable(L);
	lua_setfield(L, -2, "preload");

	lua_newtable(L);

	/* first entry in searchers list is the preload searcher */
	lua_pushvalue(L, -2);
	lua_pushcclosure(L, pllua_package_preload_search, 1);
	lua_seti(L, -2, 1);

	/* second entry in searchers list is the permitted-package searcher */
	/* this operates outside the sandbox so we don't close it over sandbox.package */
	lua_pushcfunction(L, pllua_package_allowed_search);
	lua_seti(L, -2, 2);

	lua_setfield(L, -2, "searchers");

	return 1;
}

static struct luaL_Reg sandbox_funcs[] = {
	/* from this file */
	{ "load", pllua_t_load },
	/* "require" is set from package.require */
	/* from elog.c */
    { "print", pllua_p_print },
	/* from error.c */
    { "pcall", pllua_t_pcall },
    { "xpcall", pllua_t_xpcall },
    {NULL, NULL}
};

/*
 * Whitelist the standard lua funcs that we allow into the sandbox.
 */
static struct luaL_Reg sandbox_lua_funcs[] = {
	/* base lib */
	{ "assert", NULL },
	{ "collectgarbage", NULL },
	{ "error", NULL },
	{ "getmetatable", NULL },
	{ "ipairs", NULL },
	{ "next", NULL },
	{ "pairs", NULL },
	{ "rawequal", NULL },
	{ "rawlen", NULL },
	{ "rawget", NULL },
	{ "rawset", NULL },
	{ "select", NULL },
	{ "setmetatable", NULL },
	{ "tonumber", NULL },
	{ "tostring", NULL },
	{ "type", NULL },
	{ NULL, NULL }
};

/*
 * List of packages to expose to the sandbox by default
 */
struct namepair { const char *name; const char *newname; };
static struct namepair sandbox_packages[] = {
	{ "coroutine", NULL },
	{ "string", NULL },
	{ "utf8", NULL },
	{ "table", NULL },
	{ "math", NULL },
	{ "pllua.spi", "spi" },
	{ "pllua.pgtype", "pgtype" },
	{ "pllua.server", "server" },
	{ NULL, NULL }
};
static struct namepair sandbox_allow_packages[] = {
	{ "pllua.numeric", NULL },
	{ NULL, NULL }
};

/*
 * These funcs appear as trusted.func outside the sandbox, for management
 * purposes.
 *
 * trusted.require("module", ["newname"])
 *    -- as if _ENV.newname = module   was done inside the sandbox (the
 *       actual 'require "module"' is done outside)
 *
 * trusted.allow("module", ["newname"])
 *    -- allow  require "newname"  to work inside the sandbox
 *       note that "module" WILL be loaded immediately (outside)
 *
 * trusted.remove("newname")
 *    -- remove the module from the sandbox; INEFFECTIVE if code has already
 *       been run inside.
 *
 * modules "require"d outside of the sandbox are not exposed as global
 * variables inside it unless specified with require or allow. However,
 * anything a module stores inside itself, including references to other
 * modules, will be accessible if the module is.
 *
 * CAVEAT SUPERUSER: it will be very hard to ensure that any given loaded
 * module doesn't expose the real global table, its functions, or dangerous
 * packages to the untrusted code.
 */

static int pllua_trusted_require(lua_State *L)
{
	luaL_checkstring(L, 1);
	luaL_optstring(L, 2, NULL);
	if (lua_isnoneornil(L, 2))
		lua_pushvalue(L, 1);
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX);
	lua_pushvalue(L, 2);
	lua_getglobal(L, "require");
	lua_pushvalue(L, 1);
	lua_call(L, 1, 1);
	lua_settable(L, -3);
	return 0;
}

static int pllua_bind_one_value(lua_State *L)
{
	lua_pushvalue(L, lua_upvalueindex(1));
	return 1;
}

static int pllua_trusted_allow(lua_State *L)
{
	luaL_checkstring(L, 1);
	luaL_optstring(L, 2, NULL);
	if (lua_isnoneornil(L, 2))
		lua_pushvalue(L, 1);
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX_ALLOW);
	lua_pushvalue(L, 2);
	lua_getglobal(L, "require");
	lua_pushvalue(L, 1);
	lua_call(L, 1, 1);
	lua_pushcclosure(L, pllua_bind_one_value, 1);
	lua_settable(L, -3);
	return 0;
}

static int pllua_trusted_remove(lua_State *L)
{
	luaL_checkstring(L, 1);
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX_ALLOW);
	lua_pushvalue(L, 1);
	lua_pushnil(L);
	lua_settable(L, -3);
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX);
	lua_pushvalue(L, 1);
	lua_pushnil(L);
	lua_settable(L, -3);
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX_LOADED);
	lua_pushvalue(L, 1);
	lua_pushnil(L);
	lua_settable(L, -3);
	return 0;
}

static struct luaL_Reg trusted_funcs[] = {
	{ "require", pllua_trusted_require },
	{ "allow", pllua_trusted_allow },
	{ "remove", pllua_trusted_remove },
	{ NULL, NULL }
};

int pllua_open_trusted(lua_State *L)
{
	const luaL_Reg *p;
	const struct namepair *np;
	lua_settop(L,0);
	/* create the package table itself: index 1 */
	luaL_newlibtable(L, trusted_funcs);
	luaL_setfuncs(L, trusted_funcs, 0);

	/* create the "permitted package" table */
	lua_newtable(L);
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX_ALLOW);
	lua_setfield(L, 1, "permit");

	/* create the trusted sandbox: index 2 */
	lua_newtable(L);
	for (p = sandbox_lua_funcs; p->name; ++p)
	{
		lua_getglobal(L, p->name);
		lua_setfield(L, 2, p->name);
	}
	lua_getglobal(L, "_VERSION");
	lua_setfield(L, 2, "_VERSION");
	lua_getglobal(L, "_PLVERSION");
	lua_setfield(L, 2, "_PLVERSION");
	lua_pushvalue(L, 2);
	lua_setfield(L, 2, "_G");
	luaL_setfuncs(L, sandbox_funcs, 0);
	lua_pushvalue(L, 2);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX);
	lua_pushvalue(L, 2);
	lua_setfield(L, 1, "sandbox");

	/* create the infrastructure of the sandbox module system */
	luaL_requiref(L, "pllua.trusted.package", pllua_open_trusted_package, 0);
	/* the package becomes the "package" global in the sandbox */
	lua_getfield(L, -1, "require");
	lua_setfield(L, 2, "require");
	lua_setfield(L, 2, "package");

	/* require standard modules into the sandbox */
	for (np = sandbox_packages; np->name; ++np)
	{
		lua_pushcfunction(L, pllua_trusted_allow);
		lua_pushstring(L, np->name);
		lua_call(L, 1, 0);
		lua_getfield(L, -1, "require");
		lua_pushstring(L, np->name);
		lua_call(L, 1, 1);
		lua_setfield(L, -2, np->newname ? np->newname : np->name);
	}
	for (np = sandbox_allow_packages; np->name; ++np)
	{
		lua_pushcfunction(L, pllua_trusted_allow);
		lua_pushstring(L, np->name);
		lua_call(L, 1, 0);
	}

	/* create and install the minimal trusted "os" library */
	luaL_requiref(L, "pllua.trusted.os", pllua_open_trusted_os, 0);
	lua_pop(L,1);
	lua_pushcfunction(L, pllua_trusted_allow);
	lua_pushstring(L, "pllua.trusted.os");
	lua_pushstring(L, "os");
	lua_call(L, 2, 0);
	lua_getfield(L, -1, "require");
	lua_pushstring(L, "os");
	lua_call(L, 1, 1);
	lua_setfield(L, -2, "os");

	lua_pushvalue(L, 1);
	return 1;
}
