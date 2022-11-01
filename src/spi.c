/* spi.c */

#include "pllua.h"

#include "access/htup_details.h"
#if PG_VERSION_NUM >= 110000
#include "access/xact.h"
#endif
#include "commands/trigger.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "parser/analyze.h"
#include "parser/parse_param.h"

#if PG_VERSION_NUM >= 110000
#define PortalGetHeapMemory(portal) ((portal)->portalContext)
#endif

/*
 * plpgsql uses 10. We have a bit more overhead per queue fill since we
 * start/stop SPI and do a bunch of data copies, so a larger value seems good.
 * However, above 50 the returns are very small and the potential for memory
 * problems increases.
 *
 * The fetch count can be set per-statement.
 */
#define DEFAULT_FETCH_COUNT 50

typedef struct pllua_spi_statement {
	SPIPlanPtr plan;
	bool kept;
	bool cursor_plan;
	int fetch_count;   /* only used for private cursors */
	int nparams;
	int param_types_len;
	Oid *param_types;
	MemoryContext mcxt;
} pllua_spi_statement;

/*
 * This is an object not a refobject since it references no memory other than
 * the Portal, which has its own memory context already.
 *
 * But, we have to allow for the possibility that the portal will be ripped out
 * from under us, e.g. by transaction end, explicit CLOSE, or whatever. So we
 * use the same trick as for activations (with the slight variation that we use
 * a weak table for tracking, since unlike with activations we expect to shed
 * references to no-longer-needed cursors).
 *
 * We also track whether the cursor is considered "private" to us (meaning that
 * it hasn't been exposed anywhere that the user would see, e.g. we're keeping it
 * in a closure for a rows() iterator). Since the position of such a cursor isn't
 * visible to others, we can prefetch.
 */
typedef struct pllua_spi_cursor {
	Portal portal;  /* or null if closed or dead */
	MemoryContextCallback *cb;  /* allocated in PortalContext */
	lua_State *L; /* needed by callback */
	int fetch_count;   /* only used for private cursors */
	bool is_ours;   /* we created (and will close) it? */
	bool is_private;  /* nobody else should be touching it */
	bool is_live;  /* cleared by callback */
} pllua_spi_cursor;

static pllua_spi_cursor *pllua_newcursor(lua_State *L);
static void pllua_cursor_setportal(lua_State *L, int nd,
								   pllua_spi_cursor *curs,
								   Portal portal, bool is_ours);

/*
 * Pushes up to four entries on the stack - beware!
 */
static void pllua_spi_alloc_argspace(lua_State *L,
									 int nargs,
									 Datum **values,
									 bool **isnull,
									 Oid **argtypes,
									 pllua_typeinfo ***typeinfos)
{
	if (values)
		*values = lua_newuserdata(L, nargs * sizeof(Datum));
	if (isnull)
		*isnull = lua_newuserdata(L, nargs * sizeof(bool));
	if (argtypes)
		*argtypes = lua_newuserdata(L, nargs * sizeof(Oid));
	if (typeinfos)
		*typeinfos = lua_newuserdata(L, nargs * sizeof(pllua_typeinfo *));
}

static bool pllua_spi_enter(lua_State *L)
{
	bool readonly = pllua_get_cur_act_readonly(L);
	ASSERT_PG_CONTEXT;
	SPI_connect();
#if PG_VERSION_NUM >= 100000
	{
		pllua_activation_record *pact = &(pllua_getinterpreter(L)->cur_activation);
		if (pact->fcinfo && CALLED_AS_TRIGGER(pact->fcinfo))
			SPI_register_trigger_data((TriggerData *) pact->fcinfo->context);
	}
#endif
	return readonly;
}

static int pllua_spi_is_readonly(lua_State *L)
{
	bool readonly = pllua_get_cur_act_readonly(L);
	lua_pushboolean(L, readonly);
	return 1;
}

static void pllua_spi_exit(lua_State *L)
{
	SPI_finish();
}

/*
 * This creates the result but does not copy the data into the proper memory
 * context; see pllua_spi_save_result for that.
 *
 * args: light[tuptab] nrows [table baseidx]
 * returns: typeinfo table baseidx
 */
int pllua_spi_prepare_result(lua_State *L)
{
	SPITupleTable *tuptab = lua_touserdata(L, 1);
	lua_Integer nrows = lua_tointeger(L, 2);
	TupleDesc tupdesc = tuptab->tupdesc;
	lua_Integer base = 1;
	lua_Integer i;

	if (!lua_istable(L, 3))
	{
		lua_settop(L, 3);
		lua_createtable(L, nrows, 0);
		lua_replace(L, 3);
	}
	else
		base = 1 + lua_tointeger(L, 4);

	if (tupdesc->tdtypeid == RECORDOID && tupdesc->tdtypmod < 0)
		pllua_newtypeinfo_raw(L, tupdesc->tdtypeid, tupdesc->tdtypmod, tupdesc);
	else
	{
		lua_pushcfunction(L, pllua_typeinfo_lookup);
		lua_pushinteger(L, (lua_Integer) tupdesc->tdtypeid);
		lua_pushinteger(L, (lua_Integer) tupdesc->tdtypmod);
		lua_call(L, 2, 1);
	}

	for (i = 0; i < nrows; ++i)
	{
		HeapTuple htup = tuptab->vals[i];
		HeapTupleHeader h = htup->t_data;
		pllua_datum *d;

		/* htup might be in on-disk format or datum format. Force datum format. */
		HeapTupleHeaderSetDatumLength(h, htup->t_len);
		HeapTupleHeaderSetTypeId(h, tupdesc->tdtypeid);
		HeapTupleHeaderSetTypMod(h, tupdesc->tdtypmod);

		d = pllua_newdatum(L, -1, (Datum)0);
		/* we intentionally do not detoast anything here, see savedatum */
		d->value = PointerGetDatum(h);
		lua_rawseti(L, 3, i+base);
	}

	lua_pushvalue(L, 3);
	lua_pushinteger(L, base+nrows-1);
	lua_setfield(L, -2, "n");

	lua_pushinteger(L, base);

	return 3;
}

/*
 * stack: ... typeinfo table base
 */
static void pllua_spi_save_result(lua_State *L, lua_Integer nrows)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));
	pllua_typeinfo *t = *(void **)lua_touserdata(L, -3);
	lua_Integer base = lua_tointeger(L, -1);
	lua_Integer i;

	/* we rely on the fact that rawgeti won't throw */

	for (i = 0; i < nrows; ++i)
	{
		pllua_datum *d;
		lua_rawgeti(L, -2, i+base);
		d = lua_touserdata(L, -1);
		pllua_savedatum(L, d, t);
		lua_pop(L,1);
	}

	MemoryContextSwitchTo(oldcontext);
}



static int pllua_cursor_options(lua_State *L, int nd, int *fetch_count)
{
	int n = 0;
	int isint = 0;
	int flag = 0;
	if (lua_isnoneornil(L, nd))
		return 0;
	luaL_checktype(L, nd, LUA_TTABLE);
	lua_getfield(L, nd, "scroll");
	/*
	 * support scroll = false as if it were no_scroll = true, because not doing
	 * so is too confusing. But note that specifying neither has a different
	 * effect.
	 */
	if (!lua_isnil(L, -1))
	{
		if (lua_toboolean(L, -1))
			flag |= CURSOR_OPT_SCROLL;
		else
			flag |= CURSOR_OPT_NO_SCROLL;
	}
	lua_pop(L, 1);
	lua_getfield(L, nd, "no_scroll");
	flag |= (lua_toboolean(L, -1)) ? CURSOR_OPT_NO_SCROLL : 0;
	lua_pop(L, 1);
	lua_getfield(L, nd, "hold");
	flag |= (lua_toboolean(L, -1)) ? CURSOR_OPT_HOLD : 0;
	lua_pop(L, 1);
	lua_getfield(L, nd, "fast_start");
	flag |= (lua_toboolean(L, -1)) ? CURSOR_OPT_FAST_PLAN : 0;
	lua_pop(L, 1);
	lua_getfield(L, nd, "custom_plan");
	flag |= (lua_toboolean(L, -1)) ? CURSOR_OPT_CUSTOM_PLAN : 0;
	lua_pop(L, 1);
	lua_getfield(L, nd, "generic_plan");
	flag |= (lua_toboolean(L, -1)) ? CURSOR_OPT_GENERIC_PLAN : 0;
	lua_pop(L, 1);
	lua_getfield(L, nd, "fetch_count");
	n = lua_tointegerx(L, -1, &isint);
	if (isint && n >= 1 && n < 10000000)
		*fetch_count = n;
	lua_pop(L, 1);
	return flag;
}

static int pllua_spi_prepare_recursion = -1;
static post_parse_analyze_hook_type pllua_spi_prev_parse_hook = NULL;

#if PG_VERSION_NUM >= 140000
static void pllua_spi_prepare_checkparam_hook(ParseState *pstate,
											  Query *query,
											  JumbleState *jstate)
{
	if (pllua_spi_prepare_recursion == 1)
		check_variable_parameters(pstate, query);
	if (pllua_spi_prev_parse_hook)
		pllua_spi_prev_parse_hook(pstate, query, jstate);
}
#else
static void pllua_spi_prepare_checkparam_hook(ParseState *pstate,
											  Query *query)
{
	if (pllua_spi_prepare_recursion == 1)
		check_variable_parameters(pstate, query);
	if (pllua_spi_prev_parse_hook)
		pllua_spi_prev_parse_hook(pstate, query);
}
#endif

static void pllua_spi_prepare_parser_setup_hook(ParseState *pstate, void *arg)
{
	pllua_spi_statement *stmt = arg;
#if PG_VERSION_NUM >= 150000
	setup_parse_variable_parameters(pstate, &stmt->param_types, &stmt->param_types_len);
#else
	parse_variable_parameters(pstate, &stmt->param_types, &stmt->param_types_len);
#endif
}

static pllua_spi_statement *pllua_spi_make_statement(lua_State *L,
													 const char *str,
													 int nargs_known,
													 Oid *argtypes,
													 int opts)
{
	MemoryContext mcxt = AllocSetContextCreate(CurrentMemoryContext,
											   "PL/Lua SPI statement object",
											   ALLOCSET_SMALL_SIZES);
	MemoryContext oldcontext = MemoryContextSwitchTo(mcxt);
	pllua_spi_statement *stmt = palloc0(sizeof(pllua_spi_statement));
	int i;

	ASSERT_PG_CONTEXT;

	stmt->mcxt = mcxt;
	stmt->nparams = 0;
	stmt->fetch_count = 0;

	if (nargs_known > 0)
	{
		stmt->param_types_len = nargs_known;
		stmt->param_types = palloc(nargs_known * sizeof(Oid));
		memcpy(stmt->param_types, argtypes, nargs_known * sizeof(Oid));
	}
	else
	{
		/* we have to preallocate this to get it in the right context */
		stmt->param_types_len = 16;  /* wholly arbitrary */
		stmt->param_types = palloc0(16 * sizeof(Oid));
	}

	/*
	 * GAH. To do parameter type checking properly, we have to arrange to
	 * enable our own global post-parse hook transiently.
	 */
	if (pllua_spi_prepare_recursion != 0)
		elog(ERROR, "pllua: recursive entry into prepare!"); /* paranoia */
	PG_TRY();
	{
		++pllua_spi_prepare_recursion;

		stmt->plan = SPI_prepare_params(str,
										pllua_spi_prepare_parser_setup_hook,
										stmt,
										opts);

		--pllua_spi_prepare_recursion;
	}
	PG_CATCH();
	{
		--pllua_spi_prepare_recursion;
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (!stmt->plan)
		elog(ERROR, "spi error: %s", SPI_result_code_string(SPI_result));

	for (i = stmt->param_types_len; i > 0; --i)
	{
		if (stmt->param_types[i - 1] != 0)
		{
			stmt->nparams = i;
			break;
		}
	}

	stmt->cursor_plan = SPI_is_cursor_plan(stmt->plan);

	MemoryContextSwitchTo(oldcontext);

	return stmt;
}

/*
 * prepare(cmd,[{argtypes}, [{flag=true,...,fetch_count=n}])
 *
 * argtypes are given as text or typeinfo.
 * flags: "scroll","no_scroll","fast_start","custom_plan","generic_plan"
 */
static int pllua_spi_prepare(lua_State *L)
{
	const char *str = lua_tostring(L, 1);
	int fetch_count = 0;
	int opts = pllua_cursor_options(L, 3, &fetch_count);
	void **volatile p;
	int i;
	int nargs = 0;
	Oid d_argtypes[100];
	Oid *argtypes = d_argtypes;

	if (pllua_ending)
		luaL_error(L, "cannot call SPI during shutdown");

	if (nargs > 99)
		pllua_spi_alloc_argspace(L, nargs, NULL, NULL, &argtypes, NULL);

	lua_settop(L, 2);

	/* make the plan object - index 3 */
	p = pllua_newrefobject(L, PLLUA_SPI_STMT_OBJECT, NULL, true);

	nargs = 0;
	if (!lua_isnoneornil(L, 2))
	{
		for(i = 1; ; ++i)
		{
			pllua_typeinfo *t;

			if (lua_geti(L, 2, i) == LUA_TNIL)
				break;

			if (lua_isstring(L, -1))
			{
				lua_pushcfunction(L, pllua_typeinfo_parsetype);
				lua_pushvalue(L, -2);
				lua_call(L, 1, 1);
				if (lua_isnil(L, -1))
					luaL_error(L, "unknown type '%s'", lua_tostring(L, -2));
				lua_remove(L, -2);
			}

			t = pllua_totypeinfo(L, -1);
			if (!t)
				luaL_error(L, "unexpected object type in argtypes list");

			argtypes[nargs++] = t->typeoid;
		}
	}

	PLLUA_TRY();
	{
		pllua_spi_statement *stmt;

		pllua_spi_enter(L);

		stmt = pllua_spi_make_statement(L, str, nargs, argtypes, opts);

		/* reparent everything */
		SPI_keepplan(stmt->plan);
		stmt->kept = true;
		stmt->fetch_count = fetch_count;
		MemoryContextSetParent(stmt->mcxt, pllua_get_memory_cxt(L));
		*p = stmt;

		pllua_spi_exit(L);
	}
	PLLUA_CATCH_RETHROW();

	lua_getuservalue(L, 3);

	{
		pllua_spi_statement *stmt = *p;

		for(i = 0; i < stmt->nparams; ++i)
		{
			pllua_typeinfo *t;

			/* unused params will have InvalidOid here */
			if (OidIsValid(stmt->param_types[i]))
			{
				lua_pushcfunction(L, pllua_typeinfo_lookup);
				lua_pushinteger(L, (lua_Integer) stmt->param_types[i]);
				lua_call(L, 1, 1);
				t = pllua_totypeinfo(L, -1);
				if (!t)
					luaL_error(L, "unexpected type in paramtypes list: %d", (lua_Integer) stmt->param_types[i]);

				lua_rawseti(L, -2, i+1);
			}
		}
	}

	lua_pushvalue(L, 3);
	return 1;
}

/*
 * args: light[values] light[isnull] light[argtypes] argtable arg...
 *
 * argtypes are as determined by the parser, may not match the actual type of
 * arg.
 */
int pllua_spi_convert_args(lua_State *L)
{
	Datum *values = lua_touserdata(L, 1);
	bool *isnull = lua_touserdata(L, 2);
	Oid *argtypes = lua_touserdata(L, 3);
	int nargs = lua_gettop(L) - 4;
	int argbase = 5;
	int i;

	for (i = 0; i < nargs; ++i)
	{
		if (!lua_isnil(L, argbase+i) && OidIsValid(argtypes[i]))
		{
			pllua_typeinfo *dt;
			pllua_datum *d;
			lua_pushvalue(L, argbase+i);
			d = pllua_toanydatum(L, -1, &dt);
			/* not already an unexploded datum of correct type? */
			if (!d ||
				dt->typeoid != argtypes[i] ||
				dt->obsolete || dt->modified ||
				d->modified)
			{
				if (d)
					lua_pop(L, 1);  /* discard typeinfo */
				lua_pushcfunction(L, pllua_typeinfo_lookup);
				lua_pushinteger(L, (lua_Integer) argtypes[i]);
				lua_call(L, 1, 1);
				lua_insert(L, -2);
				lua_call(L, 1, 1);
				d = pllua_toanydatum(L, -1, &dt);
			}
			/* it better be the right type now */
			if (!d || dt->typeoid != argtypes[i])
				luaL_error(L, "inconsistent value type in SPI parameter list");
			lua_pop(L, 1); /* discard typeinfo */
			/*
			 * holding a reference here means that d remains valid even though
			 * it's no longer on the stack
			 */
			lua_rawseti(L, 4, i+1);
			values[i] = d->value;
			isnull[i] = false;
		}
		else
		{
			values[i] = (Datum)0;
			isnull[i] = true;
		}
	}
	return 0;
}

static ParamListInfo
pllua_spi_init_paramlist(int nargs, Datum *values, bool *isnull, Oid *types)
{
	ParamListInfo paramLI;
	int			i;

	ASSERT_PG_CONTEXT;

	paramLI = (ParamListInfo) palloc0(offsetof(ParamListInfoData, params) +
									  nargs * sizeof(ParamExternData));
	/* we have static list of params, so no hooks needed */
	paramLI->paramFetch = NULL;
	paramLI->paramFetchArg = NULL;
#if PG_VERSION_NUM >= 110000
	paramLI->paramCompile = NULL;
	paramLI->paramCompileArg = NULL;
#endif
	paramLI->parserSetup = NULL;
	paramLI->parserSetupArg = NULL;

	paramLI->numParams = nargs;

#if PG_VERSION_NUM >= 90600 && PG_VERSION_NUM < 110000
	paramLI->paramMask = NULL;
#endif

	for (i = 0; i < nargs; i++)
	{
		ParamExternData *prm = &paramLI->params[i];

		prm->value = values[i];
		prm->isnull = isnull[i];
		prm->pflags = PARAM_FLAG_CONST;
		prm->ptype = types[i];
	}

	return paramLI;
}

/*
 * spi.execute_count(cmd, count, arg...) returns {rows...}
 * also stmt:execute_count(count, arg...)
 *
 */
static int pllua_spi_execute_count(lua_State *L)
{
	void **p = pllua_torefobject(L, 1, PLLUA_SPI_STMT_OBJECT);
	const char *str = lua_tostring(L, 1);
	int nargs = lua_gettop(L) - 2;
	int argbase = 3;
	Datum d_values[100];
	bool d_isnull[100];
	Oid d_argtypes[100];
	Datum *values = d_values;
	bool *isnull = d_isnull;
	Oid *argtypes = d_argtypes;
	lua_Integer count_param = luaL_optinteger(L, 2, 0);
	long count;
	volatile lua_Integer nrows = -1;
	int i;

	if (!str && !p)
		luaL_error(L, "incorrect argument type for execute, string or statement expected");

	if (count_param == 0)
		count = (long) Min(LUA_MAXINTEGER-1, LONG_MAX-1);
	else if (count_param < 0 || count_param > LUA_MAXINTEGER-1 || count_param > LONG_MAX-1)
		luaL_error(L, "requested number of rows is out of range");
	else
		count = (long) count_param;

	if (pllua_ending)
		luaL_error(L, "cannot call SPI during shutdown");

	if (nargs > 99)
		pllua_spi_alloc_argspace(L, nargs, &values, &isnull, &argtypes, NULL);

	/* check encoding of query string */
	if (str)
		pllua_verify_encoding(L, str);

	/*
	 * If we don't have a prepared stmt, then extract argtypes where we have
	 * definite info (i.e. only when the parameter is actually a datum).
	 */
	if (!p)
	{
		for (i = 0; i < nargs; ++i)
		{
			argtypes[i] = 0;
			if (lua_type(L, argbase+i) == LUA_TUSERDATA)
			{
				pllua_typeinfo *dt;
				pllua_datum *d = pllua_toanydatum(L, argbase+i, &dt);
				if (d)
				{
					argtypes[i] = dt->typeoid;
					lua_pop(L, 1);
				}
			}
		}
	}

	/* we're going to re-push all the args, better have space */
	luaL_checkstack(L, 40+nargs, NULL);
	lua_createtable(L, nargs, 0);  /* table to hold refs to arg datums */

	PLLUA_TRY();
	{
		bool readonly = pllua_spi_enter(L);
		pllua_spi_statement *stmt = p ? *p : NULL;
		ParamListInfo paramLI = NULL;
		int rc;

		if (!stmt)
			stmt = pllua_spi_make_statement(L, str, nargs, argtypes, 0);

		if (stmt->nparams != nargs)
			elog(ERROR, "pllua: wrong number of arguments to SPI query: expected %d got %d", stmt->nparams, nargs);

		pllua_pushcfunction(L, pllua_spi_convert_args);
		lua_pushlightuserdata(L, values);
		lua_pushlightuserdata(L, isnull);
		lua_pushlightuserdata(L, stmt->param_types);
		lua_pushvalue(L, -5);
		for (i = 0; i < nargs; ++i)
		{
			lua_pushvalue(L, argbase+i);
		}
		pllua_pcall(L, 4+nargs, 0, 0);

		if (nargs > 0)
			paramLI = pllua_spi_init_paramlist(nargs, values, isnull, stmt->param_types);

		rc = SPI_execute_plan_with_paramlist(stmt->plan, paramLI, readonly, count);
		if (rc >= 0)
		{
			nrows = SPI_processed;
			if (SPI_tuptable)
			{
				/*
				 * Blessing the tupdesc of the result turns out to be a bad
				 * idea if we can avoid it; in a long-running backend the
				 * tupdescs can really pile up.
				 */
				pllua_pushcfunction(L, pllua_spi_prepare_result);
				lua_pushlightuserdata(L, SPI_tuptable);
				lua_pushinteger(L, nrows);
				pllua_pcall(L, 2, 3, 0);

				pllua_spi_save_result(L, nrows);
				lua_pop(L, 1);
			}
			else
				lua_pushinteger(L, nrows);
		}
		else
			elog(ERROR, "spi error: %s", SPI_result_code_string(rc));

		/*
		 * If we made our own statement, we didn't save it so it goes away here
		 */

		pllua_spi_exit(L);
	}
	PLLUA_CATCH_RETHROW();

	return 1;
}

/*
 * spi.execute(cmd, arg...) returns {rows...}
 * also stmt:execute(arg...)
 *
 * simple shim to insert the count arg
 */
static int pllua_spi_execute(lua_State *L)
{
	luaL_checkany(L, 1);
	lua_pushcfunction(L, pllua_spi_execute_count);
	lua_insert(L, 1);
	lua_pushnil(L);
	lua_insert(L, 3);
	lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
	return lua_gettop(L);
}

/*
 * c:open(cmd, arg...)
 * c:open(stmt, arg...)
 *
 */
static int pllua_spi_cursor_open(lua_State *L)
{
	pllua_spi_cursor *curs = pllua_checkobject(L, 1, PLLUA_SPI_CURSOR_OBJECT);
	void **p = pllua_torefobject(L, 2, PLLUA_SPI_STMT_OBJECT);
	pllua_spi_statement *stmt = p ? *p : NULL;
	const char *str = lua_tostring(L, 2);
	const char *name = NULL;
	int nargs = lua_gettop(L) - 2;
	int argbase = 3;
	Datum d_values[100];
	bool d_isnull[100];
	Oid d_argtypes[100];
	Datum *values = d_values;
	bool *isnull = d_isnull;
	Oid *argtypes = d_argtypes;
	volatile Portal portal;
	int i;

	if (!str && !p)
		luaL_error(L, "incorrect argument type for cursor open, string or statement expected");

	if (curs->portal)
		luaL_error(L, "cursor is already open");

	if (pllua_ending)
		luaL_error(L, "cannot call SPI during shutdown");

	if (stmt && !stmt->cursor_plan)
		luaL_error(L, "invalid statement for cursor");

	if (nargs > 99)
		pllua_spi_alloc_argspace(L, nargs, &values, &isnull, &argtypes, NULL);

	/* check encoding of query string */
	if (str)
		pllua_verify_encoding(L, str);

	lua_getuservalue(L, 1);
	lua_getfield(L, -1, "name");
	name = lua_tostring(L, -1);
	lua_pop(L, 1);

	/*
	 * If we don't have a prepared stmt, then extract argtypes where we have
	 * definite info (i.e. only when the parameter is actually a datum).
	 */
	if (!stmt)
	{
		for (i = 0; i < nargs; ++i)
		{
			argtypes[i] = 0;
			if (lua_type(L, argbase+i) == LUA_TUSERDATA)
			{
				pllua_typeinfo *dt;
				pllua_datum *d = pllua_toanydatum(L, argbase+i, &dt);
				if (d)
				{
					argtypes[i] = dt->typeoid;
					lua_pop(L, 1);
				}
			}
		}
	}

	/* we're going to re-push all the args, better have space */
	luaL_checkstack(L, 40+nargs, NULL);
	lua_createtable(L, nargs, 0);

	PLLUA_TRY();
	{
		bool readonly = pllua_spi_enter(L);
		ParamListInfo paramLI = NULL;

		if (!stmt)
		{
			stmt = pllua_spi_make_statement(L, str, nargs, argtypes, 0);
			if (!stmt->cursor_plan)
				elog(ERROR, "pllua: invalid query for cursor");
		}

		if (stmt->nparams != nargs)
			elog(ERROR, "pllua: wrong number of arguments to SPI query: expected %d got %d", stmt->nparams, nargs);

		pllua_pushcfunction(L, pllua_spi_convert_args);
		lua_pushlightuserdata(L, values);
		lua_pushlightuserdata(L, isnull);
		lua_pushlightuserdata(L, stmt->param_types);
		lua_pushvalue(L, -5);
		for (i = 0; i < nargs; ++i)
		{
			lua_pushvalue(L, argbase+i);
		}
		pllua_pcall(L, 4+nargs, 0, 0);

		if (nargs > 0)
			paramLI = pllua_spi_init_paramlist(nargs, values, isnull, stmt->param_types);

		portal = SPI_cursor_open_with_paramlist(name, stmt->plan, paramLI, readonly);

		/*
		 * If we made our own statement, we didn't save it so it goes away here
		 * The portal does _not_ go away - it's not tied to SPI.
		 */

		pllua_spi_exit(L);
	}
	PLLUA_CATCH_RETHROW();

	/*
	 * Treat the new cursor as ours until told otherwise, but not private
	 * (caller does that if appropriate)
	 */
	pllua_cursor_setportal(L, 1, curs, portal, true);
	lua_pushvalue(L, 1);
	return 1;
}

/*
 * s:getcursor(args)  returns cursor
 *
 * Doesn't allow setting the name.
 *
 *  = return spi.newcursor():open(self,args)
 */
static int pllua_spi_stmt_getcursor(lua_State *L)
{
	pllua_newcursor(L);
	lua_insert(L, 1);
	lua_pushcfunction(L, pllua_spi_cursor_open);
	lua_insert(L, 1);
	lua_call(L, lua_gettop(L) - 1, 1);
	return 1;
}


/*
 * c:fetch(n [,dir])  -- returns rows
 *
 * dir = 'forward', 'backward', 'absolute', 'relative'
 */
static FetchDirection pllua_spi_cursor_direction(lua_State *L, int nd)
{
	const char *str = luaL_optstring(L, nd, "forward");
	switch (*str)
	{
		case 'f': if (strcmp(str, "forward") == 0) return FETCH_FORWARD; else break;
		case 'b': if (strcmp(str, "backward") == 0) return FETCH_BACKWARD; else break;
		case 'a': if (strcmp(str, "absolute") == 0) return FETCH_ABSOLUTE; else break;
		case 'r': if (strcmp(str, "relative") == 0) return FETCH_RELATIVE; else break;
		case 'p': if (strcmp(str, "prior") == 0) return FETCH_BACKWARD; else break;
		case 'n': if (strcmp(str, "next") == 0) return FETCH_FORWARD; else break;
	}
	luaL_error(L, "unknown fetch direction '%s'", str);
}

static int pllua_spi_cursor_fetch(lua_State *L)
{
	pllua_spi_cursor *curs = pllua_checkobject(L, 1, PLLUA_SPI_CURSOR_OBJECT);
	int64 count = luaL_optinteger(L, 2, 1);
	FetchDirection dir = pllua_spi_cursor_direction(L, 3);

	if (pllua_ending)
		luaL_error(L, "cannot call SPI during shutdown");

	if (!curs->portal || !curs->is_live)
		luaL_error(L, "attempting to fetch from a closed cursor");

	PLLUA_TRY();
	{
		int64 nrows;

		pllua_spi_enter(L);

		SPI_scroll_cursor_fetch(curs->portal, dir, count);
		nrows = SPI_processed;
		if (SPI_tuptable)
		{
			pllua_pushcfunction(L, pllua_spi_prepare_result);
			lua_pushlightuserdata(L, SPI_tuptable);
			lua_pushinteger(L, nrows);
			pllua_pcall(L, 2, 3, 0);

			pllua_spi_save_result(L, nrows);
			lua_pop(L, 1);
		}
		else
			lua_pushinteger(L, nrows);

		pllua_spi_exit(L);
	}
	PLLUA_CATCH_RETHROW();

	return 1;
}

static int pllua_spi_cursor_move(lua_State *L)
{
	pllua_spi_cursor *curs = pllua_checkobject(L, 1, PLLUA_SPI_CURSOR_OBJECT);
	int64 count = luaL_optinteger(L, 2, 1);
	FetchDirection dir = pllua_spi_cursor_direction(L, 3);

	if (pllua_ending)
		luaL_error(L, "cannot call SPI during shutdown");

	if (!curs->portal || !curs->is_live)
		luaL_error(L, "attempting to fetch from a closed cursor");

	PLLUA_TRY();
	{
		int64 nrows;

		pllua_spi_enter(L);

		SPI_scroll_cursor_move(curs->portal, dir, count);
		nrows = SPI_processed;
		lua_pushinteger(L, nrows);

		pllua_spi_exit(L);
	}
	PLLUA_CATCH_RETHROW();

	return 1;
}

static int pllua_stmt_cursor_ok(lua_State *L)
{
	pllua_spi_statement *stmt = *pllua_checkrefobject(L, 1, PLLUA_SPI_STMT_OBJECT);
	lua_pushboolean(L, stmt->cursor_plan);
	return 1;
}

static int pllua_stmt_numargs(lua_State *L)
{
	pllua_spi_statement *stmt = *pllua_checkrefobject(L, 1, PLLUA_SPI_STMT_OBJECT);
	lua_pushinteger(L, stmt->nparams);
	return 1;
}

static int pllua_stmt_argtype(lua_State *L)
{
	pllua_spi_statement *stmt = *pllua_checkrefobject(L, 1, PLLUA_SPI_STMT_OBJECT);
	int i = luaL_checkinteger(L, 2);
	int nparams = stmt->nparams;
	if (i < 1 || i > nparams)
		luaL_error(L, "parameter %d out of range", i);
	lua_getuservalue(L, 1);
	lua_rawgeti(L, -1, i);
	return 1;
}

static int pllua_stmt_gc(lua_State *L)
{
	void **p = pllua_torefobject(L, 1, PLLUA_SPI_STMT_OBJECT);
	pllua_spi_statement *stmt = p ? *p : NULL;

	ASSERT_LUA_CONTEXT;

	if (!p)
		return 0;

	*p = NULL;
	if (!stmt)
		return 0;

	PLLUA_TRY();
	{
		if (stmt->kept && stmt->plan)
			SPI_freeplan(stmt->plan);
		MemoryContextDelete(stmt->mcxt);
	}
	PLLUA_CATCH_RETHROW();

	return 0;
}

int pllua_cursor_cleanup_portal(lua_State *L)
{
	Portal portal = lua_touserdata(L, 1);

	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_PORTALS);
	lua_pushnil(L);
	lua_rawsetp(L, -2, portal);
	lua_pop(L, 1);

	return 0;
}

static void pllua_cursor_cb(void *arg)
{
	pllua_spi_cursor *curs = arg;

	if (curs && curs->is_live)
	{
		lua_State *L = curs->L;
		Portal portal = curs->portal;

		curs->is_live = false;
		if (curs->cb)
			curs->cb->arg = NULL;
		curs->cb = NULL;
		curs->portal = NULL;

		/*
		 * we got here from pg, in a memory context reset. Since we shouldn't ever
		 * have allowed ourselves far enough into pg for that to happen while in
		 * lua context, assert that fact.
		 */
		ASSERT_PG_CONTEXT;
		/*
		 * we'd better ignore any (unlikely) lua error here, since that's safer
		 * than raising an error into pg here
		 */
		if (portal
			&& pllua_cpcall(L, pllua_cursor_cleanup_portal, portal))
			pllua_poperror(L);
	}
}

static pllua_spi_cursor *pllua_newcursor(lua_State *L)
{
	pllua_spi_cursor *curs = pllua_newobject(L, PLLUA_SPI_CURSOR_OBJECT,
											 sizeof(pllua_spi_cursor), true);
	curs->L = L;
	curs->portal = NULL;
	curs->cb = NULL;
	curs->fetch_count = 0;
	curs->is_ours = false;
	curs->is_private = false;
	curs->is_live = false;

	return curs;
}

/*
 * Associate a Portal with a cursor object. If the cursor was open, closes the
 * portal (if we own it) or dissociates from it (if it's not ours).
 *
 * A cursor object without a portal just has various data in its uservalue
 * (e.g. a name or (unexecuted) query). Setting a portal turns it into a
 * live open cursor.
 *
 * Caller must NOT set an open portal on a new cursor unless it has verified
 * that no entry already exists in reg[PORTALS] for this portal!
 */
static void pllua_cursor_setportal(lua_State *L, int nd,
								   pllua_spi_cursor *curs,
								   Portal portal, bool is_ours)
{
	Portal oldportal = curs->portal;

	nd = lua_absindex(L, nd);

	if (oldportal)
	{
		/*
		 * Dissociate everything from the portal first. If for some reason
		 * something throws an error here, we'll no longer have any references
		 * to the portal anywhere.
		 */
		if (curs->cb)
			curs->cb->arg = NULL;

		lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_PORTALS);
		lua_pushnil(L);
		lua_rawsetp(L, -2, oldportal);
		lua_pop(L, 1);

		curs->portal = NULL;
	}

	if ((oldportal && curs->is_ours) || portal)
	{
		PLLUA_TRY();
		{
			if (curs->is_ours && oldportal)
				SPI_cursor_close(oldportal);
			if (portal)
				curs->cb = MemoryContextAlloc(PortalGetHeapMemory(portal),
											  sizeof(MemoryContextCallback));
		}
		PLLUA_CATCH_RETHROW();
	}

	if (portal)
	{
		curs->cb->func = pllua_cursor_cb;
		curs->cb->arg = NULL;
		curs->L = L;

		lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_PORTALS);
		lua_pushvalue(L, nd);
		lua_rawsetp(L, -2, portal);
		lua_pop(L, 1);

		curs->portal = portal;
		curs->cb->arg = curs;
		curs->is_live = true;
		curs->is_ours = is_ours;
		curs->is_private = false;

		MemoryContextRegisterResetCallback(PortalGetHeapMemory(portal), curs->cb);
	}
}

static Portal pllua_spi_findportal(lua_State *L, const char *name)
{
	volatile Portal portal;
	PLLUA_TRY();
	{
		portal = SPI_cursor_find(name);
	}
	PLLUA_CATCH_RETHROW();
	return portal;
}
/*
 * findcursor('name')  - return cursor object given a portal name
 */
static int pllua_spi_findcursor(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	Portal portal = pllua_spi_findportal(L, name);

	if (!portal)
		return 0;

	pllua_verify_encoding(L, name);

	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_PORTALS);
	if (lua_rawgetp(L, -1, portal) == LUA_TUSERDATA)
	{
		pllua_spi_cursor *curs = pllua_toobject(L, -1, PLLUA_SPI_CURSOR_OBJECT);
		if (!curs || curs->portal != portal)
			luaL_error(L, "portal lookup mismatch");
		return 1;
	}
	else
	{
		pllua_spi_cursor *curs = pllua_newcursor(L);

		/* store the portal name */
		lua_getuservalue(L, -1);
		lua_pushvalue(L, 1);
		lua_setfield(L, -2, "name");
		lua_pop(L, 1);

		pllua_cursor_setportal(L, -1, curs, portal, false);
		return 1;
	}
}

/*
 * newcursor(['name']) - unlike findcursor, always returns a cursor; if name
 * was not specified or no portal exists with the given name, returns an
 * unopened cursor.
 */
int pllua_spi_newcursor(lua_State *L)
{
	const char *name = luaL_optstring(L, 1, NULL);

	if (name)
	{
		lua_pushcfunction(L, pllua_spi_findcursor);
		lua_pushvalue(L, 1);
		lua_call(L, 1, 1);
		if (!lua_isnil(L, -1))
			return 1;
	}

	pllua_newcursor(L);

	if (name)
	{
		/* store the portal name (encoding already checked) */
		lua_getuservalue(L, -1);
		lua_pushvalue(L, 1);
		lua_setfield(L, -2, "name");
		lua_pop(L, 1);
	}

	/* no actual portal yet */
	return 1;
}


/*
 * s:close
 */
static int pllua_cursor_close(lua_State *L)
{
	pllua_spi_cursor *curs = pllua_checkobject(L, 1, PLLUA_SPI_CURSOR_OBJECT);

	if (!curs->portal || !curs->is_live)
		return 0;

	curs->is_ours = true;
	pllua_cursor_setportal(L, 1, curs, NULL, false);

	return 0;
}

/*
 * s:isopen
 */
static int pllua_cursor_isopen(lua_State *L)
{
	pllua_spi_cursor *curs = pllua_checkobject(L, 1, PLLUA_SPI_CURSOR_OBJECT);

	lua_pushboolean(L, (curs->portal && curs->is_live));
	return 1;
}

/*
 * s:own
 */
static int pllua_cursor_own(lua_State *L)
{
	pllua_spi_cursor *curs = pllua_checkobject(L, 1, PLLUA_SPI_CURSOR_OBJECT);

	lua_settop(L, 1);

	if (!curs->portal || !curs->is_live)
		return 1;

	curs->is_ours = true;
	return 1;
}

/*
 * s:disown
 */
static int pllua_cursor_disown(lua_State *L)
{
	pllua_spi_cursor *curs = pllua_checkobject(L, 1, PLLUA_SPI_CURSOR_OBJECT);

	lua_settop(L, 1);

	if (!curs->portal || !curs->is_live)
		return 1;

	curs->is_ours = false;
	return 1;
}

/*
 * s:isowned
 */
static int pllua_cursor_isowned(lua_State *L)
{
	pllua_spi_cursor *curs = pllua_checkobject(L, 1, PLLUA_SPI_CURSOR_OBJECT);

	lua_pushboolean(L, curs->is_ours);
	return 1;
}

/*
 * s:name
 */
int pllua_cursor_name(lua_State *L)
{
	pllua_spi_cursor *curs = pllua_checkobject(L, 1, PLLUA_SPI_CURSOR_OBJECT);

	if (curs->portal && curs->is_live && curs->portal->name)
		lua_pushstring(L, curs->portal->name);
	else
	{
		lua_getuservalue(L, 1);
		lua_getfield(L, -1, "name");
	}
	return 1;
}

static int pllua_cursor_gc(lua_State *L)
{
	pllua_spi_cursor *curs = pllua_toobject(L, 1, PLLUA_SPI_CURSOR_OBJECT);

	ASSERT_LUA_CONTEXT;

	if (!curs || !curs->is_live || !curs->portal)
		return 0;

	pllua_cursor_setportal(L, 1, curs, NULL, false);

	return 0;
}

static int pllua_cursor_meta_close(lua_State *L)
{
	pllua_spi_cursor *curs = pllua_toobject(L, 1, PLLUA_SPI_CURSOR_OBJECT);

	if (!curs || !curs->portal || !curs->is_live || !curs->is_ours)
		return 0;

	pllua_cursor_setportal(L, 1, curs, NULL, false);

	return 0;
}


/*
 * rows iterator
 *
 * upvalue 1: cursor object
 * upvalue 2: current queue pos
 * upvalue 3: current queue size
 */
static int pllua_spi_stmt_rows_iter(lua_State *L)
{
	pllua_spi_cursor *curs = pllua_checkobject(L, lua_upvalueindex(1), PLLUA_SPI_CURSOR_OBJECT);
	int fetch_count = curs->is_private ? curs->fetch_count : 1;
	int qpos = lua_tointeger(L, lua_upvalueindex(2));
	int qlen = lua_tointeger(L, lua_upvalueindex(3));
	/*
	 * If the cursor disappeared from under us, then throw error even if
	 * there's stuff in the queue; otherwise things would be too confusing and
	 * unpredictable
	 */
	if (!curs->portal || !curs->is_live)
		luaL_error(L, "cannot iterate a closed cursor");
	if (fetch_count == 0)
		fetch_count = DEFAULT_FETCH_COUNT;
	if (fetch_count > 1 && qpos < qlen)
	{
		pllua_get_user_field(L, lua_upvalueindex(1), "q");
		lua_geti(L, -1, ++qpos);
		lua_remove(L, -2);
	}
	else
	{
		lua_pushcfunction(L, pllua_spi_cursor_fetch);
		lua_pushvalue(L, lua_upvalueindex(1));
		lua_pushinteger(L, fetch_count);
		lua_call(L, 2, 1);
		if (lua_isnil(L,-1))
			luaL_error(L, "cursor fetch returned nil");
		if (fetch_count > 1)
		{
			lua_pushvalue(L, -1);
			pllua_set_user_field(L, lua_upvalueindex(1), "q");
			qpos = 1;
			lua_getfield(L, -1, "n");
			qlen = lua_tointeger(L, -1);
			lua_replace(L, lua_upvalueindex(3));
		}
		lua_geti(L, -1, 1);
	}
	if (lua_isnil(L, -1))
	{
		if (curs->is_private)
		{
			lua_pushcfunction(L, pllua_cursor_close);
			lua_pushvalue(L, lua_upvalueindex(1));
			lua_call(L, 1, 0);
			lua_pushnil(L);
			lua_replace(L, lua_upvalueindex(1));
		}
		lua_pushnil(L);
		return 1;
	}
	if (fetch_count > 1)
	{
		lua_pushinteger(L, qpos);
		lua_replace(L, lua_upvalueindex(2));
	}
	return 1;
}

/*
 * s:rows(args)  returns iterator, nil, nil, cursor
 *
 */
static int pllua_spi_stmt_rows(lua_State *L)
{
	void **p = pllua_torefobject(L, 1, PLLUA_SPI_STMT_OBJECT);
	pllua_spi_cursor *curs = pllua_newcursor(L);

	if (p)
	{
		pllua_spi_statement *stmt = *p;
		curs->fetch_count = stmt->fetch_count;
	}

	lua_insert(L, 1);
	lua_pushcfunction(L, pllua_spi_cursor_open);
	lua_insert(L, 1);
	lua_call(L, lua_gettop(L) - 1, 1);

	curs->is_private = 1;
	lua_pushvalue(L, -1);
	lua_pushinteger(L, 0);
	lua_pushinteger(L, 0);
	lua_pushcclosure(L, pllua_spi_stmt_rows_iter, 3);
	lua_pushnil(L);
	lua_pushnil(L);
	lua_pushvalue(L, -4);
	return 4;
}

/*
 * c:rows()  returns iterator, nil, nil
 *
 */
static int pllua_cursor_rows(lua_State *L)
{
	pllua_checkobject(L, 1, PLLUA_SPI_CURSOR_OBJECT);

	lua_settop(L, 1);
	lua_pushinteger(L, 0);
	lua_pushinteger(L, 0);
	lua_pushcclosure(L, pllua_spi_stmt_rows_iter, 3);
	lua_pushnil(L);
	lua_pushnil(L);
	return 3;
}

#if PG_VERSION_NUM >= 110000

static int pllua_spi_xact(lua_State *L, bool commit)
{
	pllua_interpreter *interp = pllua_getinterpreter(L);
	if (interp->cur_activation.atomic)
		luaL_error(L, "cannot commit or rollback in this context");
	if (IsSubTransaction())
		luaL_error(L, "cannot commit or rollback from inside a subtransaction");

	PLLUA_TRY();
	{
		SPI_connect_ext(SPI_OPT_NONATOMIC);
		if (commit)
			SPI_commit();
		else
			SPI_rollback();
		SPI_start_transaction();
		SPI_finish();
	}
	PLLUA_CATCH_RETHROW();

	return 0;
}

static int pllua_spi_commit(lua_State *L)
{
	return pllua_spi_xact(L, true);
}

static int pllua_spi_rollback(lua_State *L)
{
	return pllua_spi_xact(L, false);
}

#endif

static int pllua_spi_is_atomic(lua_State *L)
{
	pllua_interpreter *interp = pllua_getinterpreter(L);
	lua_pushboolean(L, interp->cur_activation.atomic ? 1 : 0);
	return 1;
}

static struct luaL_Reg spi_funcs[] = {
	{ "execute", pllua_spi_execute },
	{ "execute_count", pllua_spi_execute_count },
	{ "prepare", pllua_spi_prepare },
	{ "readonly", pllua_spi_is_readonly },
	{ "findcursor", pllua_spi_findcursor },
	{ "newcursor", pllua_spi_newcursor },
	{ "rows", pllua_spi_stmt_rows },
#if PG_VERSION_NUM >= 110000
	{ "commit", pllua_spi_commit },
	{ "rollback", pllua_spi_rollback },
#endif
	{ "is_atomic", pllua_spi_is_atomic },
	{ NULL, NULL }
};

static struct luaL_Reg spi_cursor_methods[] = {
	{ "fetch", pllua_spi_cursor_fetch },
	{ "move", pllua_spi_cursor_move },
	{ "own", pllua_cursor_own },
	{ "isowned", pllua_cursor_isowned },
	{ "disown", pllua_cursor_disown },
	{ "close", pllua_cursor_close },
	{ "isopen", pllua_cursor_isopen },
	{ "name", pllua_cursor_name },
	{ "rows", pllua_cursor_rows },
	{ "open", pllua_spi_cursor_open },
	{ NULL, NULL }
};
static struct luaL_Reg spi_cursor_mt[] = {
	{ "__gc", pllua_cursor_gc },
	{ "__close", pllua_cursor_meta_close },
	{ NULL, NULL }
};

static int pllua_spi_noop(lua_State *L)
{
	lua_settop(L, 1); return 1;  /* return first arg */
}

static int pllua_spi_noop_true(lua_State *L)
{
	lua_pushboolean(L, 1); return 1; /* return true */
}

static struct luaL_Reg spi_stmt_methods[] = {
	{ "save", pllua_spi_noop },
	{ "issaved", pllua_spi_noop_true },
	{ "execute", pllua_spi_execute },
	{ "execute_count", pllua_spi_execute_count },
	{ "getcursor", pllua_spi_stmt_getcursor },
	{ "rows", pllua_spi_stmt_rows },
	{ "numargs", pllua_stmt_numargs },
	{ "argtype", pllua_stmt_argtype },
	{ "cursor_ok", pllua_stmt_cursor_ok },
	{ NULL, NULL }
};
static struct luaL_Reg spi_stmt_mt[] = {
	{ "__gc", pllua_stmt_gc },
	{ "__call", pllua_spi_execute },
	{ NULL, NULL }
};

int pllua_open_spi(lua_State *L)
{
	if (pllua_spi_prepare_recursion == -1)
	{
		pllua_spi_prev_parse_hook = post_parse_analyze_hook;
		post_parse_analyze_hook = pllua_spi_prepare_checkparam_hook;
		pllua_spi_prepare_recursion = 0;
	}

	pllua_newmetatable(L, PLLUA_SPI_STMT_OBJECT, spi_stmt_mt);
	luaL_newlib(L, spi_stmt_methods);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	/* make a weak table to hold portals: light[Portal] = cursor object */
	pllua_new_weak_table(L, "v", "spi portal registry table");
	lua_pop(L, 1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_PORTALS);

	pllua_newmetatable(L, PLLUA_SPI_CURSOR_OBJECT, spi_cursor_mt);
	luaL_newlib(L, spi_cursor_methods);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	lua_newtable(L);
	luaL_setfuncs(L, spi_funcs, 0);

	/*
	 * Inherit the pllua.elog module via a metatable
	 */
	lua_newtable(L);
	lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
	lua_getfield(L, -1, "pllua.elog");
	lua_setfield(L, -3, "__index");
	lua_pushboolean(L, 1);
	lua_setfield(L, -3, "__metatable");
	lua_pop(L, 1);
	lua_setmetatable(L, -2);

	return 1;
}
