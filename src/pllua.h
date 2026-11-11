/*
 * pllua.h
 */

#ifndef PLLUA_H
#define PLLUA_H

#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"

#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/palloc.h"

#include "miscadmin.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#define PLLUA_VERSION_STR "2.0"
#define PLLUA_VERSION_NUM 200
#define PLLUA_REVISION_STR "2.0012"
#define PLLUA_REVISION_NUM 20012

/* PG version dependencies */
#include "pllua_pgver.h"

#if LUA_VERSION_NUM == 501

/* assume Lua 5.1 is actually luajit */
#include "pllua_luajit.h"

#elif LUA_VERSION_NUM >= 503

/* Lua version dependencies */
#include "pllua_luaver.h"

#else
#error Unsupported Lua version (only Lua 5.3+ and Luajit are supported)
#endif

/* Work around compiler issues. */

#ifndef __has_attribute
#define __has_attribute(x_) 0
#endif

#if __has_attribute(__fallthrough__)
#define FALLTHROUGH __attribute__((__fallthrough__))
#else
#define FALLTHROUGH /* FALLTHROUGH */
#endif

LUA_API int   (lua_error) (lua_State *L) pg_attribute_noreturn();
LUALIB_API int (luaL_argerror) (lua_State *L, int arg, const char *extramsg) pg_attribute_noreturn();
LUALIB_API int (luaL_typeerror) (lua_State *L, int arg, const char *tname) pg_attribute_noreturn();
LUALIB_API int (luaL_error) (lua_State *L, const char *fmt, ...) pg_attribute_noreturn();

/*
 * Define how we want to handle int8 values.
 *
 * If nothing here is set, we will treat int8 as any other ordinary datum,
 * which means it'll be passed through unchanged, will stringify to its
 * ordinary decimal representation, will work as a json key or value and so on,
 * but won't be accessible to arithmetic or comparisons (or use as a table key)
 * in the lua code except by conversion to pgtype.numeric and use of the
 * numeric module.
 *
 * If PLLUA_INT8_OK is defined, which we do if the underlying Lua has a 64-bit
 * integer subtype (lua 5.3 with lua_Integer being 64 bits), then we convert
 * int8 to a Lua value and back, which makes it available for direct arithmetic
 * and use as a table key, while still stringifying to the same value and
 * working as a json key or value.
 *
 * If PLLUA_INT8_LUAJIT_HACK is defined, which we never do automatically at
 * present but only if using luajit _and_ USE_INT8_CDATA is defined on the
 * command line, then we convert int8 to a luajit cdata. This breaks
 * stringification and json key/value usage, does not enable use as a table
 * key, but makes arithmetic possible. This all seems like a major loss on
 * balance, hence why it's not the default.
 *
 * NO_LUAJIT turns off all attempts to use luajit-specific features.
 *
 */

#if LUA_VERSION_NUM >= 503
#if LUA_MAXINTEGER == PG_INT64_MAX
#define PLLUA_INT8_OK
#endif
#endif

#if LUAJIT_VERSION_NUM > 0 && defined(USE_INT8_CDATA) && !defined(NO_LUAJIT)
#define PLLUA_INT8_LUAJIT_HACK
#else
#undef PLLUA_INT8_LUAJIT_HACK
#endif

/*
 * Track what error handling context we're in, to try and detect any violations
 * of error-handling protocol (lua errors thrown through pg catch blocks and
 * vice-versa).
 */
typedef enum
{
	PLLUA_CONTEXT_PG,
	PLLUA_CONTEXT_LUA
} pllua_context_type;

extern pllua_context_type pllua_context;
extern bool pllua_pending_error;

#define ASSERT_PG_CONTEXT Assert(pllua_context == PLLUA_CONTEXT_PG)
#define ASSERT_LUA_CONTEXT Assert(pllua_context == PLLUA_CONTEXT_LUA)

void pllua_pending_error_violation(lua_State *L) pg_attribute_noreturn();

static inline pllua_context_type
pllua_setcontext(lua_State *L, pllua_context_type newctx)
{
	pllua_context_type oldctx = pllua_context;
	if (unlikely(pllua_pending_error)
		&& L
		&& oldctx == PLLUA_CONTEXT_LUA
		&& newctx == PLLUA_CONTEXT_PG)
		pllua_pending_error_violation(L);
	pllua_context = newctx;
	return oldctx;
}

/*
 * Abbreviate the most common form of catch block.
 */
#define PLLUA_TRY() do {												\
	pllua_context_type _pllua_oldctx = pllua_setcontext(L, PLLUA_CONTEXT_PG); \
	MemoryContext _pllua_oldmcxt = CurrentMemoryContext;				\
	PG_TRY()

#define PLLUA_TRY_ERROK() do {												\
	pllua_context_type _pllua_oldctx = pllua_setcontext(NULL, PLLUA_CONTEXT_PG); \
	MemoryContext _pllua_oldmcxt = CurrentMemoryContext;				\
	PG_TRY()

#define PLLUA_CATCH_RETHROW()											\
	PG_CATCH();															\
	{ pllua_setcontext(NULL, _pllua_oldctx); pllua_rethrow_from_pg(L, _pllua_oldmcxt); } \
	PG_END_TRY(); pllua_setcontext(NULL, _pllua_oldctx); } while(0)

#define pllua_debug(L_, ...)											\
	do { if (pllua_context==PLLUA_CONTEXT_PG) elog(DEBUG1, __VA_ARGS__); \
		else pllua_debug_lua(L_, __VA_ARGS__); } while(0)

#define PLLUA_CHECK_PG_STACK_DEPTH()							\
	do { if (stack_is_too_deep()) luaL_error(L, "stack depth exceeded"); } while (0)

/*
 * Describes one call to the top-level handler.
 */
struct pllua_interpreter;

typedef struct pllua_activation_record
{
	FunctionCallInfo fcinfo;
	Datum		retval;

	/* if fcinfo is null, we're validating or doing inline */
	InlineCodeBlock *cblock;
	Oid			validate_func;

	bool		atomic;
	bool		trusted;

	/* registry ref for current error if any */
	int			active_error;

	/* for error context stuff */
	struct pllua_interpreter *interp;
	const char *err_text;

	/*
	 * If not NULL, an SPI connection with the transition tables of the
	 * trigger being called is open for the whole call, and this is its
	 * procedure memory context. Cursors are opened on that connection; see
	 * pllua_spi_cursor_open().
	 */
	MemoryContext trig_spi_cxt;
} pllua_activation_record;

typedef struct pllua_cache_inval
{
	bool		inval_type;
	bool		inval_rel;
	bool		inval_cast;
	Oid			inval_typeoid;
	Oid			inval_reloid;
	Oid			pad0_;
} pllua_cache_inval;

/*
 * Top-level data for one interpreter. We keep a hashtable of these per
 * user_id (for trusted mode isolation). We keep a pointer to this in the Lua
 * state and use it to access the current activation fields (which are
 * saved/restored on recursive entries).
 */
typedef struct pllua_interpreter
{
	lua_State  *L;				/* The interpreter proper */

	lua_Alloc	allocf;
	void	   *alloc_ud;		/* saved ud value for allocator */

	MemoryContext mcxt;
	MemoryContext emcxt;

	ErrorData  *edata;

	Oid			user_id;
	bool		db_ready;

	unsigned long gc_debt;		/* estimated additional GC debt */

	/* state below must be saved/restored for recursive calls */
	pllua_activation_record cur_activation;

	/* stuff used transiently in error handling and elsewhere */
	lua_Debug	ar;
	int			errdepth;
	bool		update_errdepth;

	/* convenience copy so as not to point lightuserdata at stack */
	pllua_cache_inval inval;

	/* buffer for warning system */
	int			warncount;
	char		warnbuf[PLLUA_WARNBUF_SIZE];
} pllua_interpreter;

typedef struct pllua_interpreter_hashent
{
	Oid			user_id;		/* hash key, must be first */

	bool		trusted;
	bool		new_ident;

	pllua_interpreter *interp;
} pllua_interpreter_hashent;

/* We abuse the node system to pass this in fcinfo->context */

#define PLLUA_MAGIC 0x4c554101

typedef struct pllua_node
{
	NodeTag		type;   /* we put T_Invalid here */
	uint32		magic;
	lua_State  *L;
} pllua_node;

/*
 * We don't put this in the body of a lua userdata for error handling reasons;
 * we want to build it from pg data without involving lua too much until we're
 * ready to actually compile the function. Instead, the lua object is a pointer
 * to this with a __gc method, and the object itself is palloc'd (in its own
 * memory context). The activation records (corresponding to flinfo) are lua
 * objects that reference the funcinfo, preventing it from being GC'd while in
 * use.
 *
 * The actual lua function object is stored in the uservalue slot under key
 * light(PLLUA_FUNCTION_MEMBER).
 */
typedef struct pllua_function_info
{
	Oid			fn_oid;
	/* for revalidation checks */
	TransactionId fn_xmin;
	ItemPointerData fn_tid;

	Oid			rettype;
	bool		returns_row;
	bool		retset;
	bool		readonly;
	bool		is_trigger;
	bool		is_event_trigger;

	int			nargs;
	bool		variadic;
	bool		variadic_any;
	bool		polymorphic;
	bool		polymorphic_ret;

	Oid		   *argtypes;

	Oid			language_oid;
	bool		trusted;

	MemoryContext mcxt;

	const char *name;
} pllua_function_info;

/*
 * This is info we need to compile the function but not needed to run it.
 */
typedef struct pllua_function_compile_info
{
	pllua_function_info *func_info;

	MemoryContext mcxt;

	text	   *prosrc;

	int			nargs;
	int			nallargs;

	Oid			variadic;

	Oid		   *allargtypes;
	char	   *argmodes;
	char	  **argnames;

	bool		validate_only;		/* don't run any code when compiling */
} pllua_function_compile_info;


/* this one ends up in flinfo->fn_extra */

typedef struct pllua_func_activation
{
	lua_State  *thread;		/* non-null for a running SRF */
	bool		onstack;	/* needed for error handling */

	pllua_interpreter *interp;		/* direct access for SRF resume */

	pllua_function_info *func_info;

	bool		resolved;

	bool		polymorphic;
	bool		variadic_call;		/* only if variadic_any */
	bool		retset;
	bool		readonly;

	Oid			rettype;
	TupleDesc	tupdesc;
	TypeFuncClass typefuncclass;
	bool		retdomain;

	int			nargs;
	Oid		   *argtypes;	/* with polymorphism resolved */

	/*
	 * this data is allocated and referenced in lua, so we need to arrange to
	 * drop it for GC when the context containing the pointer to it is reset
	 */
	lua_State  *L;
	bool		dead;
	MemoryContextCallback cb;
} pllua_func_activation;

/*
 * Body of a Datum object. typmod is usually -1 except when we got the value
 * from a source with a declared typmod (such as a column).
 */
typedef struct pllua_datum
{
	Datum		value;
	int32		typmod;
	bool		need_gc;
	bool		modified;		/* composite value has been exploded */
} pllua_datum;

/*
 * Stuff we store about types. Datum values reference this from their
 * metatables (in fact the metatable of the Datum is the uservalue of
 * this object, which also contains a reference to the object itself).
 */
typedef struct pllua_typeinfo
{
	Oid			typeoid;
	int32 		typmod;			/* only for RECORD */

	int			arity;	/* 1 for scalars, otherwise no. undropped cols */
	int			natts;	/* -1 for scalars */

	TupleDesc	tupdesc;
	Oid			reloid;		/* for named composite types */
	Oid			basetype;	/* for domains */
	Oid			elemtype;	/* for arrays */
	Oid			rangetype;	/* for ranges */
	bool		hasoid;
	bool		is_array;
	bool		is_range;
	bool		is_enum;
	bool		is_anonymous_record;
	bool		nested_unknowns;
	bool		nested_composites;

	bool		revalidate;
	bool		modified;
	bool		obsolete;

	int16		typlen;
	bool		typbyval;
	char		typalign;
	char		typdelim;
	Oid			typioparam;
	Oid			outfuncid;

	Oid			infuncid;	/* we don't look these up until we need them */
	Oid			sendfuncid;
	Oid			recvfuncid;

	FmgrInfo	outfunc;
	FmgrInfo	infunc;
	FmgrInfo	sendfunc;
	FmgrInfo	recvfunc;

	bool		coerce_typmod;		/* typmod coercions needed */
	bool		coerce_typmod_element;
	Oid			typmod_funcid;

	int32		basetypmod;			/* for domains */
	void	   *domain_extra;		/* domain_check workspace */
	ArrayMetaState array_meta;		/* array workspace */

	int16		elemtyplen;			/* for arrays only */
	bool		elemtypbyval;
	char		elemtypalign;

	Oid			fromsql;		/* fromsql(internal) returns internal */
	Oid			tosql;			/* tosql(internal) returns datum */

	/*
	 * we give this its own context, because we can't control what fmgr will
	 * dangle off the FmgrInfo structs
     */
	MemoryContext mcxt;
} pllua_typeinfo;

/*
 * are we shutting down?
 */

extern bool pllua_ending;

/*
 * Addresses used as lua registry or object keys
 *
 * Note the key is the address, not the string; the string is only for
 * debugging purposes.
 *
 * The registry looks like this (all keys are light userdata):
 *
 * global state:
 * reg[PLLUA_USERID] = int user_id for trusted, InvalidOid for untrusted
 * reg[PLLUA_TRUSTED] = boolean
 * reg[PLLUA_LANG_OID] = oid of language
 * reg[PLLUA_LAST_ERROR] = last pg error object to enter error handling
 * reg[PLLUA_RECURSIVE_ERROR] = preallocated error object
 *
 * registries for cached data:
 * reg[PLLUA_FUNCS] = { [integer oid] = funcinfo object }
 * reg[PLLUA_ACTIVATIONS] = { [light(act)] = activation object }
 * reg[PLLUA_TYPES] = { [integer oid] = typeinfo object }
 * reg[PLLUA_RECORDS] = { [integer typmod] = typeinfo object }
 * reg[PLLUA_PORTALS] = { [light(Portal)] = cursor object }
 *
 * metatables:
 * reg[PLLUA_FUNCTION_OBJECT]
 * reg[PLLUA_ACTIVATION_OBJECT]
 * reg[PLLUA_ERROR_OBJECT]
 * reg[PLLUA_TYPEINFO_OBJECT]
 * reg[PLLUA_TYPEINFO_PACKAGE_OBJECT]  (the pgtype() object itself)
 *  - datum objects and tupconv objects have dynamic metatables
 *
 * sandbox:
 * reg[PLLUA_TRUSTED_SANDBOX] = value of _ENV for trusted funcs
 * reg[PLLUA_TRUSTED_SANDBOX_LOADED] = modules loaded in sandbox
 * reg[PLLUA_TRUSTED_SANDBOX_ALLOW] = modules allowed in sandbox
 *
 * reg[PLLUA_]
 *
 *
 */

extern char PLLUA_USERID[];
extern char PLLUA_LANG_OID[];
extern char PLLUA_TRUSTED[];
extern char PLLUA_FUNCS[];
extern char PLLUA_TYPES[];
extern char PLLUA_RECORDS[];
extern char PLLUA_ACTIVATIONS[];
extern char PLLUA_PORTALS[];
extern char PLLUA_FUNCTION_OBJECT[];
extern char PLLUA_ERROR_OBJECT[];
extern char PLLUA_IDXLIST_OBJECT[];
extern char PLLUA_ACTIVATION_OBJECT[];
extern char PLLUA_MCONTEXT_OBJECT[];
extern char PLLUA_TYPEINFO_OBJECT[];
extern char PLLUA_TYPEINFO_PACKAGE_OBJECT[];
extern char PLLUA_TYPEINFO_PACKAGE_ARRAY_OBJECT[];
extern char PLLUA_TUPCONV_OBJECT[];
extern char PLLUA_TRIGGER_OBJECT[];
extern char PLLUA_EVENT_TRIGGER_OBJECT[];
extern char PLLUA_SPI_STMT_OBJECT[];
extern char PLLUA_SPI_CURSOR_OBJECT[];
extern char PLLUA_LAST_ERROR[];
extern char PLLUA_RECURSIVE_ERROR[];
extern char PLLUA_FUNCTION_MEMBER[];
extern char PLLUA_MCONTEXT_MEMBER[];
extern char PLLUA_THREAD_MEMBER[];
extern char PLLUA_TYPEINFO_MEMBER[];
extern char PLLUA_TRUSTED_SANDBOX[];
extern char PLLUA_TRUSTED_SANDBOX_LOADED[];
extern char PLLUA_TRUSTED_SANDBOX_ALLOW[];
extern char PLLUA_PGFUNC_TABLE_OBJECT[];
extern char PLLUA_TYPECONV_REGISTRY[];
extern char PLLUA_ERRCODES_TABLE[];
extern char PLLUA_PRINT_SEVERITY[];
extern char PLLUA_GLOBAL_META[];
extern char PLLUA_SANDBOX_META[];

/* functions */

/* init.c */

pllua_interpreter *pllua_getstate(bool trusted, pllua_activation_record *act);

/*
 * careful, mustn't throw
 */
static inline pllua_interpreter *
pllua_getinterpreter(lua_State *L)
{
	void *p;
	lua_getallocf(L, &p);
	return p;
}

/*
 * pllua_get_memory_cxt
 *
 * Get the memory context associated with the interpreter.
 */
static inline MemoryContext
pllua_get_memory_cxt(lua_State *L)
{
	pllua_interpreter *interp = pllua_getinterpreter(L);
	return interp->mcxt;
}

int pllua_set_new_ident(lua_State *L);
void pllua_run_extra_gc(lua_State *L, unsigned long gc_debt);
PGDLLEXPORT bool pllua_stack_is_too_deep(void);
PGDLLEXPORT void pllua_stack_depth_error(void);

extern bool pllua_track_gc_debt;
extern bool pllua_do_install_globals;

/*
 * This is a macro because we want to avoid executing (sz_) at all if not tracking
 * gc debt, since it might be a toast_datum_size call with nontrivial overhead.
 */
#define pllua_record_gc_debt(L_, sz_) \
	do { if (pllua_track_gc_debt) pllua_record_gc_debt_real(L_, (sz_)); } while (0)

static inline void
pllua_record_gc_debt_real(lua_State *L, unsigned long bytes)
{
	pllua_interpreter *interp = pllua_getinterpreter(L);
	if (interp)
		interp->gc_debt += bytes;
}

/* compile.c */

pllua_func_activation *pllua_validate_and_push(lua_State *L, FunctionCallInfo fcinfo, bool trusted);
void pllua_compile_inline(lua_State *L, const char *str, bool trusted);
int pllua_compile(lua_State *L);
int pllua_intern_function(lua_State *L);
void pllua_validate_function(lua_State *L, Oid fn_oid, bool trusted);

/* datum.c */
int pllua_open_pgtype(lua_State *L);

void pllua_verify_encoding(lua_State *L, const char *str);
bool pllua_verify_encoding_noerror(lua_State *L, const char *str);
void *pllua_palloc(lua_State *L, size_t sz);
pllua_typeinfo *pllua_totypeinfo(lua_State *L, int nd);
pllua_typeinfo *pllua_checktypeinfo(lua_State *L, int nd, bool revalidate);
pllua_datum *pllua_checkanydatum(lua_State *L, int nd, pllua_typeinfo **ti);
pllua_datum *pllua_checkdatum(lua_State *L, int nd, int td);
pllua_datum *pllua_toanydatum(lua_State *L, int nd, pllua_typeinfo **ti);
pllua_datum *pllua_todatum(lua_State *L, int nd, int td);
int pllua_typeinfo_invalidate(lua_State *L);
void pllua_savedatum(lua_State *L,
					 struct pllua_datum *d,
					 struct pllua_typeinfo *t);
void pllua_save_one_datum(lua_State *L,
						  pllua_datum *d,
						  pllua_typeinfo *t);
int pllua_value_from_datum(lua_State *L,
						   Datum value,
						   Oid typeid);
int pllua_datum_transform_fromsql(lua_State *L,
								  Datum value,
								  int nt,
								  pllua_typeinfo *t);
bool pllua_datum_from_value(lua_State *L, int nd,
							Oid typeid,
							Datum *result,
							bool *isnull,
							const char **errstr);
pllua_datum *pllua_newdatum(lua_State *L, int nt, Datum value);
int pllua_typeinfo_lookup(lua_State *L);
pllua_typeinfo *pllua_newtypeinfo_raw(lua_State *L, Oid oid, int32 typmod, TupleDesc tupdesc);
int pllua_typeinfo_parsetype(lua_State *L);
int pllua_datum_single(lua_State *L, Datum res, bool isnull, int nt, pllua_typeinfo *t);
int pllua_typeconv_invalidate(lua_State *L);
void pllua_typeinfo_check_domain(lua_State *L,
								 Datum *val, bool *isnull, int32 typmod,
								 int nt, pllua_typeinfo *t);

/* elog.c */
int pllua_open_elog(lua_State *L);
int pllua_open_print(lua_State *L);

int pllua_p_print (lua_State *L);
void pllua_debug_lua(lua_State *L, const char *msg, ...) pg_attribute_printf(2, 3);
void pllua_error(lua_State *L, const char *msg, ...) pg_attribute_noreturn();
void pllua_warning(lua_State *L, const char *msg, ...) pg_attribute_printf(2, 3);
void pllua_error_callback(void *arg);
int pllua_error_callback_location(lua_State *L);

/* DLLEXPORT so that transform modules can get at it */
PGDLLEXPORT const char *pllua_tolstring(lua_State *L, int idx, size_t *len);

/* error.c */
int pllua_open_error(lua_State *L);
ErrorData *pllua_make_recursive_error(void);
void pllua_error_cleanup(pllua_interpreter *interp, pllua_activation_record *act);

int pllua_panic(lua_State *L);
int pllua_newerror(lua_State *L);
int pllua_register_error(lua_State *L);
void pllua_poperror(lua_State *L);
void pllua_rethrow_from_lua(lua_State *L, int rc) pg_attribute_noreturn();

/* These are DLLEXPORT so that transform modules can get at them */
PGDLLEXPORT void pllua_rethrow_from_pg(lua_State *L, MemoryContext mcxt) pg_attribute_noreturn();
PGDLLEXPORT int pllua_pcall_nothrow(lua_State *L, int nargs, int nresults, int msgh);
PGDLLEXPORT int pllua_cpcall(lua_State *L, lua_CFunction func, void* arg);
PGDLLEXPORT void pllua_pcall(lua_State *L, int nargs, int nresults, int msgh);
PGDLLEXPORT int pllua_trampoline(lua_State *L);
PGDLLEXPORT void pllua_pending_error_violation(lua_State *L);

void pllua_initial_protected_call(pllua_interpreter *interp,
								  lua_CFunction func,
								  pllua_activation_record *arg);
int pllua_t_assert(lua_State *L);
int pllua_t_error(lua_State *L);
int pllua_t_pcall(lua_State *L);
int pllua_t_xpcall(lua_State *L);
int pllua_t_lpcall(lua_State *L);
int pllua_t_lxpcall(lua_State *L);

/* exec.c */

int pllua_resume_function(lua_State *L);
int pllua_call_function(lua_State *L);
int pllua_call_trigger(lua_State *L);
int pllua_call_event_trigger(lua_State *L);
int pllua_call_inline(lua_State *L);
int pllua_validate(lua_State *L);

/* jsonb.c */
int pllua_open_jsonb(lua_State *L);

/* numeric.c */
int pllua_open_numeric(lua_State *L);

/* objects.c */
int pllua_open_funcmgr(lua_State *L);

/* These are DLLEXPORT so that transform modules can get at them */
PGDLLEXPORT bool pllua_is_container(lua_State *L, int nd);
PGDLLEXPORT bool pllua_pairs_start(lua_State *L, int nd, bool noerror);
PGDLLEXPORT int pllua_pairs_next(lua_State *L);

bool pllua_isobject(lua_State *L, int nd, char *objtype);
void pllua_newmetatable(lua_State *L, char *objtype, luaL_Reg *mt);
void pllua_new_weak_table(lua_State *L, const char *mode, const char *name);
void **pllua_newrefobject(lua_State *L, char *objtype, void *value, bool uservalue);
void **pllua_torefobject(lua_State *L, int nd, char *objtype);
void *pllua_newobject(lua_State *L, char *objtype, size_t sz, bool uservalue);
void *pllua_toobject(lua_State *L, int nd, char *objtype);
void pllua_type_error(lua_State *L, char *expected) pg_attribute_noreturn();
void **pllua_checkrefobject(lua_State *L, int nd, char *objtype);
void *pllua_checkobject(lua_State *L, int nd, char *objtype);

MemoryContext pllua_newmemcontext(lua_State *L,
								  const char *name,
								  Size minsz,
								  Size initsz,
								  Size maxsz);

void pllua_set_user_field(lua_State *L, int nd, const char *field);
int pllua_get_user_field(lua_State *L, int nd, const char *field);
int pllua_get_user_subfield(lua_State *L, int nd, const char *field, const char *subfield);

int pllua_newactivation(lua_State *L);
int pllua_setactivation(lua_State *L);
void pllua_getactivation(lua_State *L, pllua_func_activation *act);
int pllua_activation_getfunc(lua_State *L);
int pllua_get_cur_act(lua_State *L);
FmgrInfo *pllua_get_cur_flinfo(lua_State *L);
bool pllua_get_cur_act_readonly(lua_State *L);
int pllua_freeactivation(lua_State *L);
int pllua_resetactivation(lua_State *L);

lua_State *pllua_activate_thread(lua_State *L, int nd, ExprContext *econtext);
void pllua_deactivate_thread(lua_State *L, pllua_func_activation *act, ExprContext *econtext);

void pllua_pgfunc_new(lua_State *L);
FmgrInfo *pllua_pgfunc_init(lua_State *L, int nd, Oid fnoid, int nargs, Oid *argtypes, Oid rettype);
void pllua_pgfunc_table_new(lua_State *L);

/* paths.c */
int pllua_open_paths(lua_State *L);

/* preload.c */
int pllua_preload_compat(lua_State *L);

/* spi.c */
int pllua_open_spi(lua_State *L);

int pllua_spi_convert_args(lua_State *L);
int pllua_spi_prepare_result(lua_State *L);
int pllua_cursor_cleanup_portal(lua_State *L);

int pllua_spi_newcursor(lua_State *L);
int pllua_cursor_name(lua_State *L);

/* time.c */
int pllua_open_time(lua_State *L);

/* trigger.c */
int pllua_open_trigger(lua_State *L);

struct TriggerData;
struct EventTriggerData;
void pllua_trigger_begin(lua_State *L, struct TriggerData *td);
void pllua_trigger_end(lua_State *L, int nd);
int pllua_push_trigger_args(lua_State *L, struct TriggerData *td);
Datum pllua_return_trigger_result(lua_State *L, int nret, int nd);

void pllua_evtrigger_begin(lua_State *L, struct EventTriggerData *td);
void pllua_evtrigger_end(lua_State *L, int nd);

/* trusted.c */
int pllua_open_trusted(lua_State *L);
int pllua_open_trusted_late(lua_State *L);

#endif
