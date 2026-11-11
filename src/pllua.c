/*
 * pllua.c: PL/Lua NG call handler
 * By Andrew "RhodiumToad" Gierth, rhodiumtoad at postgresql.org
 * Based in some part on pllua by Luis Carvalho and others
 * License: MIT license or PostgreSQL licence
 */

#include "pllua.h"

#include "commands/event_trigger.h"
#include "commands/trigger.h"
#include "executor/spi.h"

PG_MODULE_MAGIC;

/*
 * Exposed interface
 *
 * see also _PG_init in init.c
 */
PGDLLEXPORT Datum pllua_validator(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pllua_call_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pllua_inline_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum plluau_validator(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum plluau_call_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum plluau_inline_handler(PG_FUNCTION_ARGS);

static Datum pllua_common_call(FunctionCallInfo fcinfo, bool trusted);
static Datum pllua_common_inline(FunctionCallInfo fcinfo, bool trusted);
static Datum pllua_common_validator(FunctionCallInfo fcinfo, bool trusted);


/* Trusted entry points */

PG_FUNCTION_INFO_V1(pllua_validator);
Datum pllua_validator(PG_FUNCTION_ARGS)
{
	return pllua_common_validator(fcinfo, true);
}

PG_FUNCTION_INFO_V1(pllua_call_handler);
Datum pllua_call_handler(PG_FUNCTION_ARGS)
{
	return pllua_common_call(fcinfo, true);
}

PG_FUNCTION_INFO_V1(pllua_inline_handler);
Datum pllua_inline_handler(PG_FUNCTION_ARGS)
{
	return pllua_common_inline(fcinfo, true);
}

/* Untrusted entry points */

PG_FUNCTION_INFO_V1(plluau_validator);
Datum plluau_validator(PG_FUNCTION_ARGS)
{
	return pllua_common_validator(fcinfo, false);
}

PG_FUNCTION_INFO_V1(plluau_call_handler);
Datum plluau_call_handler(PG_FUNCTION_ARGS)
{
	return pllua_common_call(fcinfo, false);
}

PG_FUNCTION_INFO_V1(plluau_inline_handler);
Datum plluau_inline_handler(PG_FUNCTION_ARGS)
{
	return pllua_common_inline(fcinfo, false);
}

/* Common implementations */

static void pllua_entry_stack_check(void)
{
	check_stack_depth();
}

Datum pllua_common_call(FunctionCallInfo fcinfo, bool trusted)
{
	pllua_interpreter *volatile interp = NULL;
	pllua_activation_record act;
	pllua_func_activation *funcact = (fcinfo->flinfo) ? fcinfo->flinfo->fn_extra : NULL;
	ErrorContextCallback ecxt;

	pllua_entry_stack_check();

	act.fcinfo = fcinfo;
	act.retval = (Datum) 0;
	act.atomic = true;
	act.trusted = trusted;
	act.cblock = NULL;
	act.validate_func = InvalidOid;
	act.interp = NULL;
	act.active_error = LUA_REFNIL;
	act.err_text = NULL;
	act.trig_spi_cxt = NULL;

#if PG_VERSION_NUM >= 110000
	if (fcinfo->context && IsA(fcinfo->context, CallContext))
		act.atomic = castNode(CallContext, fcinfo->context)->atomic;
#endif

	pllua_setcontext(NULL, PLLUA_CONTEXT_PG);

	/*
	 * this catch block exists to save/restore the error context stack and
	 * allow cleanup of our internal error state when returning to PG proper
	 */
	PG_TRY();
	{
		ecxt.callback = pllua_error_callback;
		ecxt.arg = &act;
		ecxt.previous = error_context_stack;
		error_context_stack = &ecxt;

		if (funcact && funcact->thread)
			act.interp = funcact->interp;
		else
			act.interp = pllua_getstate(trusted, &act);

		interp = act.interp;

		if (funcact && funcact->thread)
		{
			/*
			 * We're resuming a value-per-call SRF, so we bypass almost
			 * everything since we don't want to, for example, compile a new
			 * version of the function halfway through a result set. We know
			 * we're in a non-first-row condition if there's an existing thread
			 * in the function activation.
			 */
			pllua_initial_protected_call(act.interp, pllua_resume_function, &act);
		}
		else if (CALLED_AS_TRIGGER(fcinfo))
		{
#if PG_VERSION_NUM >= 100000
			TriggerData *td = (TriggerData *) fcinfo->context;

			/*
			 * Portals that use the transition tables registered on an SPI
			 * connection must not outlive it, and since the November 2026
			 * minor releases SPI_finish() drops them (CVE-2026-120143).
			 * Every other SPI call of ours has its own connection, so keep
			 * one connection with the transition tables open for the whole
			 * trigger call, for cursors (e.g. spi.rows()) to be opened on.
			 * On error, the (sub)transaction abort closes it.
			 */
			if (td->tg_oldtable || td->tg_newtable)
			{
				MemoryContext oldcxt = CurrentMemoryContext;

				if (SPI_connect() != SPI_OK_CONNECT)
					elog(ERROR, "pllua: SPI_connect failed");
				if (SPI_register_trigger_data(td) != SPI_OK_TD_REGISTER)
					elog(ERROR, "pllua: SPI_register_trigger_data failed");
				act.trig_spi_cxt = CurrentMemoryContext;
				MemoryContextSwitchTo(oldcxt);
			}
#endif
			pllua_initial_protected_call(act.interp, pllua_call_trigger, &act);
			if (act.trig_spi_cxt)
			{
				act.trig_spi_cxt = NULL;
				if (SPI_finish() != SPI_OK_FINISH)
					elog(ERROR, "pllua: SPI_finish failed");
			}
		}
		else if (CALLED_AS_EVENT_TRIGGER(fcinfo))
			pllua_initial_protected_call(act.interp, pllua_call_event_trigger, &act);
		else
			pllua_initial_protected_call(act.interp, pllua_call_function, &act);
	}
	PG_CATCH();
	{
		if (interp)
			pllua_error_cleanup(interp, &act);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return act.retval;
}

Datum pllua_common_validator(FunctionCallInfo fcinfo, bool trusted)
{
	pllua_interpreter *volatile interp = NULL;
	pllua_activation_record act;
	Oid funcoid = PG_GETARG_OID(0);
	ErrorContextCallback ecxt;

	/* security checks */
	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	/*
	 * This doesn't need a stack check because the validator is careful
	 * not to execute user-supplied Lua code.
	 */

	act.fcinfo = NULL;
	act.retval = (Datum) 0;
	act.atomic = true;
	act.trusted = trusted;
	act.cblock = NULL;
	act.validate_func = funcoid;
	act.interp = NULL;
	act.active_error = LUA_REFNIL;
	act.err_text = NULL;
	act.trig_spi_cxt = NULL;

	pllua_setcontext(NULL, PLLUA_CONTEXT_PG);

	/*
	 * this catch block exists to save/restore the error context stack and
	 * allow cleanup of our internal error state when returning to PG proper
	 */
	PG_TRY();
	{
		ecxt.callback = pllua_error_callback;
		ecxt.arg = &act;
		ecxt.previous = error_context_stack;
		error_context_stack = &ecxt;

		interp = act.interp = pllua_getstate(trusted, &act);

		pllua_initial_protected_call(act.interp, pllua_validate, &act);
	}
	PG_CATCH();
	{
		if (interp)
			pllua_error_cleanup(interp, &act);
		PG_RE_THROW();
	}
	PG_END_TRY();

	PG_RETURN_VOID();
}

Datum pllua_common_inline(FunctionCallInfo fcinfo, bool trusted)
{
	pllua_interpreter *volatile interp = NULL;
	pllua_activation_record act;
	ErrorContextCallback ecxt;

	pllua_entry_stack_check();

	act.fcinfo = NULL;
	act.retval = (Datum) 0;
	act.atomic = true;
	act.trusted = trusted;
	act.cblock = (InlineCodeBlock *) PG_GETARG_POINTER(0);
	act.validate_func = InvalidOid;
	act.interp = NULL;
	act.active_error = LUA_REFNIL;
	act.err_text = "inline block entry";
	act.trig_spi_cxt = NULL;

#if PG_VERSION_NUM >= 110000
	act.atomic = act.cblock->atomic;
#endif

	pllua_setcontext(NULL, PLLUA_CONTEXT_PG);

	/* probably excess paranoia */
	if (act.cblock->langIsTrusted != act.trusted)
		elog(ERROR, "trusted state mismatch");

	/*
	 * this catch block exists to save/restore the error context stack and
	 * allow cleanup of our internal error state when returning to PG proper
	 */
	PG_TRY();
	{
		ecxt.callback = pllua_error_callback;
		ecxt.arg = &act;
		ecxt.previous = error_context_stack;
		error_context_stack = &ecxt;

		interp = act.interp = pllua_getstate(trusted, &act);

		pllua_initial_protected_call(act.interp, pllua_call_inline, &act);
	}
	PG_CATCH();
	{
		if (interp)
			pllua_error_cleanup(interp, &act);
		PG_RE_THROW();
	}
	PG_END_TRY();

	PG_RETURN_VOID();
}
