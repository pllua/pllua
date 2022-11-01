/* time.c */

#include "pllua.h"

#include "pgtime.h"
#include "catalog/pg_type.h"
#include "datatype/timestamp.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#if PG_VERSION_NUM >= 120000
#include "utils/float.h"
#endif
#if PG_VERSION_NUM >= 100000
#include "utils/fmgrprotos.h"
#endif
#include "utils/timestamp.h"

#ifndef DATETIME_MIN_JULIAN
#define DATETIME_MIN_JULIAN (0)
#endif
#ifndef DATE_END_JULIAN
#define DATE_END_JULIAN JULIAN_MAX
#endif
#ifndef IS_VALID_DATE
/* Range-check a date (given in Postgres, not Julian, numbering) */
#define IS_VALID_DATE(d) \
	((DATETIME_MIN_JULIAN - POSTGRES_EPOCH_JDATE) <= (d) && \
	 (d) < (DATE_END_JULIAN - POSTGRES_EPOCH_JDATE))
#endif

#ifdef HAVE_INT64_TIMESTAMP
#define FSEC_T_SCALE(f_) (f_)
#else
#define FSEC_T_SCALE(f_) ((int)(rint((f_) * 1000000.0)))
#endif

/* floor division assuming a positive divisor */
static inline int64
floordiv(int64 dividend, int64 divisor)
{
	return (dividend / divisor) - (dividend < 0 && (dividend % divisor) != 0);
}

/* overflow calculation */
static inline int
calc_overflow(int val, int modulus, int *nextfield)
{
	int carry = floordiv(val, modulus);
	*nextfield += carry;
	return val - (carry * modulus);
}

/*
 * Convenience function - given a stack index, is it:
 *
 *   - not convertible to a number (error)
 *   - an integer (including an integral float)
 *   - an infinity (error if inf_sign is null or already has the other sign)
 *   - a float (error if NaN or if fval is null)
 *
 * A lua_Integer might, in unusual cases such as running on luajit on a 32-bit
 * platform, not be big enough to handle things we want to use this for (such
 * as microsecond times), so use int64 instead. On 5.3+ where we have real
 * integers, we try to avoid loss of integer precision.
 *
 * Returns true for floats, false for integers. inf_sign is not changed if
 * the value is not an infinity.
 */
static bool
getnumber(lua_State *L, int idx,
		  int64 *ival, lua_Number *fval, int *inf_sign,
		  const char *diag_field)
{
	int isnum = 0;
	int isign = 0;
	lua_Integer inum;
	lua_Number num;

#if LUA_VERSION_NUM < 503
	num = lua_tonumberx(L, idx, &isnum);
	inum = (int64) num;
	*ival = inum;
	if (isnum)
	{
		if (num == (lua_Number)inum)
			return false;
		if (isinf(num))
			isign = (num < 0) ? -1 : 1;
	}
#else
	inum = lua_tointegerx(L, idx, &isnum);
	*ival = inum;
	if (isnum)
		return false;

	num = lua_tonumberx(L, idx, &isnum);
	if (isnum)
	{
		if (num == (lua_Number)(int64)num)
		{
			*ival = (int64) num;
			return false;
		}
		if (isinf(num))
			isign = (num < 0) ? -1 : 1;
	}
#endif

	if (!isnum ||
		isnan(num) ||
		(isign && (!inf_sign ||	(*inf_sign && isign != *inf_sign))) ||
		(!isign && !fval))
		luaL_error(L, "invalid value in field '%s'", diag_field);
	if (inf_sign && isign)
		*inf_sign = isign;
	else
		*fval = num;
	return true;
}

/*
 * determine_timezone_offset
 *
 * This is a corrected version of pg's DetermineTimeZoneOffset, which does not
 * correctly handle the case where the value is in the ambiguous hour but
 * already has tm_isdst set to disambiguate it.
 */
static int
determine_timezone_offset(struct pg_tm *tm, pg_tz *tzp)
{
	int			date,
				sec;
	pg_time_t	day,
				mytime,
				prevtime,
				boundary,
				beforetime,
				aftertime;
	long int	before_gmtoff,
				after_gmtoff;
	int			before_isdst,
				after_isdst;
	int			res;

	/*
	 * First, generate the pg_time_t value corresponding to the given
	 * y/m/d/h/m/s taken as GMT time.  If this overflows, punt and decide the
	 * timezone is GMT.  (For a valid Julian date, integer overflow should be
	 * impossible with 64-bit pg_time_t, but let's check for safety.)
	 */
	if (!IS_VALID_JULIAN(tm->tm_year, tm->tm_mon, tm->tm_mday))
		goto overflow;
	date = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - UNIX_EPOCH_JDATE;

	day = ((pg_time_t) date) * SECS_PER_DAY;
	if (day / SECS_PER_DAY != date)
		goto overflow;
	sec = tm->tm_sec + (tm->tm_min + tm->tm_hour * MINS_PER_HOUR) * SECS_PER_MINUTE;
	mytime = day + sec;
	/* since sec >= 0, overflow could only be from +day to -mytime */
	if (mytime < 0 && day > 0)
		goto overflow;

	/*
	 * Find the DST time boundary just before or following the target time. We
	 * assume that all zones have GMT offsets less than 24 hours, and that DST
	 * boundaries can't be closer together than 48 hours, so backing up 24
	 * hours and finding the "next" boundary will work.
	 */
	prevtime = mytime - SECS_PER_DAY;
	if (mytime < 0 && prevtime > 0)
		goto overflow;

	res = pg_next_dst_boundary(&prevtime,
							   &before_gmtoff, &before_isdst,
							   &boundary,
							   &after_gmtoff, &after_isdst,
							   tzp);
	if (res < 0)
		goto overflow;			/* failure? */

	if (res == 0)
	{
		/* Non-DST zone, life is simple */
		tm->tm_isdst = before_isdst;
		return -(int) before_gmtoff;
	}

	/*
	 * Form the candidate pg_time_t values with local-time adjustment
	 */
	beforetime = mytime - before_gmtoff;
	if ((before_gmtoff > 0 &&
		 mytime < 0 && beforetime > 0) ||
		(before_gmtoff <= 0 &&
		 mytime > 0 && beforetime < 0))
		goto overflow;
	aftertime = mytime - after_gmtoff;
	if ((after_gmtoff > 0 &&
		 mytime < 0 && aftertime > 0) ||
		(after_gmtoff <= 0 &&
		 mytime > 0 && aftertime < 0))
		goto overflow;

	/*
	 * If both before or both after the boundary time, we know what to do. The
	 * boundary time itself is considered to be after the transition, which
	 * means we can accept aftertime == boundary in the second case.
	 */
	if (beforetime < boundary && aftertime < boundary)
	{
		tm->tm_isdst = before_isdst;
		return -(int) before_gmtoff;
	}
	if (beforetime > boundary && aftertime >= boundary)
	{
		tm->tm_isdst = after_isdst;
		return -(int) after_gmtoff;
	}

	/*
	 * It's an invalid or ambiguous time due to timezone transition.
	 *
	 * In a spring-forward transition, this means the originally specified
	 * time was invalid, e.g. 2019-03-31 01:30:00 Europe/London (a time which
	 * never happened because 00:59:59 was followed by 02:00:00). If tm_isdst
	 * is set to -1, we prefer to use the "before" interpretation, under which
	 * this time will be interpreted as if 02:30:00. (The "after"
	 * interpretation would have made it 00:30:00 which would be surprising.)
	 *
	 * In a fall-back transition, the originally specified time was ambiguous,
	 * i.e. it occurred more than once. There is no principled choice here, but
	 * "after" is how the original version of this code behaved, and that seems
	 * consistent with typical mktime implementations.
	 *
	 * If tm_isdst is not -1, though, we respect that value and do not
	 * override it.
	 */
	if (tm->tm_isdst == -1)
	{
		if (beforetime > aftertime)
		{
			tm->tm_isdst = before_isdst;
			return -(int) before_gmtoff;
		}
		else
		{
			tm->tm_isdst = after_isdst;
			return -(int) after_gmtoff;
		}
	}
	else if (tm->tm_isdst == before_isdst)
		return -(int) before_gmtoff;
	else
		return -(int) after_gmtoff;

overflow:
	/* Given date is out of range, so assume UTC */
	tm->tm_isdst = 0;
	return 0;
}


/*
 * _tosql function
 *
 * Upvalue 1 is the typeinfo, upvalue 2 is the type oid.
 *
 * We accept a lua value if it is a table or userdata which we can index into
 * for field names like "year" etc. We won't get here in the case of a single
 * datum value, so a userdata param is assumed not to be a datum.
 *
 * The completely preposterous length of this function is mostly down to the
 * lack of any kind of usable internal interfaces for the PG date/time types
 * (and even the external interfaces are badly flawed).
 */
static int
pllua_time_tosql(lua_State *L)
{
	pllua_typeinfo *t = *pllua_torefobject(L, lua_upvalueindex(1), PLLUA_TYPEINFO_OBJECT);
	pllua_datum *d;
	Oid oid = (Oid) lua_tointeger(L, lua_upvalueindex(2));
	int nargs = lua_gettop(L);
	TimestampTz tsval;
	DateADT dateresult;
	Datum iresult;
	Datum result = 0;
	static struct pg_tm ztm = { 0 };
	struct pg_tm tm;
	const char *tzname = NULL;
	pg_tz *tz = NULL;
	int64 gmtoff = 0;
	int64 tmpint = 0;
	lua_Number tmpflt = 0.0;
	int64 microsecs = 0;
	int64 epoch_microsecs = 0;
	int found_year = 0;
	int found_mon = 0;
	int found_mday = 0;
	int found_hour = 0;
	int found_min = 0;
	int found_sec = 0;
	int found_epoch = 0;
	int found_tz = 0;
	int found_gmtoff = 0;
	int inf_sign = 0;

	/* for now, decline if not exactly 1 indexable arg. */
	if (nargs != 1 ||
		!(lua_type(L, 1) == LUA_TTABLE ||
		  (lua_type(L, 1) == LUA_TUSERDATA && luaL_getmetafield(L, 1, "__index") != LUA_TNIL)))
		return 0;

	lua_settop(L, 1);

	/*
	 * Note: for most uses of pg_tm, tm_year has the actual year (not offset
	 * by 1900) and tm_mon starts at 1 not 0, matching the Lua convention.
	 * isdst defaults to -1 and not 0 if not found in the input.
	 */

	tm = ztm;
	tm.tm_isdst = -1;

#define TMGET(name_,fname_) \
	do { if (lua_getfield(L, 1, name_) != LUA_TNIL) \
	{ \
		getnumber(L, -1, &tmpint, NULL, &inf_sign, name_); \
		tm.tm_##fname_ = tmpint; \
		found_##fname_ = 1; \
	} } while (0)

	TMGET("year", year);
	TMGET("month",  mon);
	TMGET("day",  mday);
	TMGET("hour", hour);
	TMGET("min",  min);
	/* "sec" handled specially below */

#undef TMGET

	if (lua_getfield(L, 1, "isdst") != LUA_TNIL)
		tm.tm_isdst = lua_toboolean(L, -1) ? 1 : 0;

	lua_settop(L, 1);

	/*
	 * Accept a fractional part as any combination of:
	 *  sec = float
	 *  millisecs = number
	 *  microsecs = number
	 */
	if (lua_getfield(L, 1, "sec") != LUA_TNIL)
	{
		if (getnumber(L, -1, &tmpint, &tmpflt, &inf_sign, "sec"))
		{
			double fisec = 0;
			double frac = modf(fabs(tmpflt), &fisec);

			if (tmpflt < 0)
			{
				tm.tm_sec = -((int) fisec + 1);
				microsecs = 1000000 - (int) lrint(frac * 1000000.0);
			}
			else
			{
				tm.tm_sec = (int) fisec;
				microsecs = (int) lrint(frac * 1000000.0);
			}
		}
		else
			tm.tm_sec = tmpint;

		found_sec = 1;
	}

	/*
	 * Fields "msec" and "usec" are offsets (which may be negative and/or
	 * larger than one second) from the time specified by the other values.
	 */
	if (lua_getfield(L, 1, "msec") != LUA_TNIL)
	{
		if (getnumber(L, -1, &tmpint, &tmpflt, &inf_sign, "msec"))
			microsecs += (int64) llrint(tmpflt * 1000.0);
		else
			microsecs += (tmpint * INT64CONST(1000));
	}
	if (lua_getfield(L, 1, "usec") != LUA_TNIL)
	{
		if (getnumber(L, -1, &tmpint, &tmpflt, &inf_sign, "usec"))
			microsecs += (int64) llrint(tmpflt);
		else
			microsecs += tmpint;
	}

	/*
	 * In place of YMDhms, accept any one of:
	 *
	 * epoch = number
	 * epoch_msec = number
	 * epoch_usec = number
	 */

	if (lua_getfield(L, 1, "epoch") != LUA_TNIL)
	{
		if (getnumber(L, -1, &tmpint, &tmpflt, &inf_sign, "epoch"))
			epoch_microsecs = (int64) llrint(tmpflt * 1000000.0);
		else
			epoch_microsecs = tmpint * INT64CONST(1000000);
		++found_epoch;
	}
	if (lua_getfield(L, 1, "epoch_msec") != LUA_TNIL)
	{
		if (getnumber(L, -1, &tmpint, &tmpflt, &inf_sign, "epoch_msec"))
			epoch_microsecs = (int64) llrint(tmpflt * 1000.0);
		else
			epoch_microsecs = tmpint * INT64CONST(1000);
		++found_epoch;
	}
	if (lua_getfield(L, 1, "epoch_usec") != LUA_TNIL)
	{
		if (getnumber(L, -1, &tmpint, &tmpflt, &inf_sign, "epoch_usec"))
			epoch_microsecs = (int64) llrint(tmpflt);
		else
			epoch_microsecs = tmpint;
		++found_epoch;
	}

	lua_settop(L, 1);

	switch (lua_getfield(L, 1, "timezone"))
	{
		case LUA_TNIL:
			break;

		case LUA_TBOOLEAN:
			if (lua_toboolean(L, -1))
				found_tz = 1;
			break;

		case LUA_TSTRING:
			{
				int tzoff = 0;
				found_tz = 1;
				tzname = lua_tostring(L, -1);
				if (tzname && DecodeTimezone((char *) tzname, &tzoff) == 0)
				{
					gmtoff = -tzoff;
					found_gmtoff = 1;
				}
			}
			break;

		default:
			getnumber(L, -1, &gmtoff, NULL, NULL, "timezone");
			found_gmtoff = 1;
			break;
	}

	/* input done, check validity of everything */

	if (found_epoch > 1)
		luaL_error(L, "cannot specify multiple epoch fields");
	else if (found_epoch)
	{
		if (found_year || found_mon || found_mday)
			luaL_error(L, "cannot specify both epoch and date fields");
		if (found_hour || found_min || found_sec)
			luaL_error(L, "cannot specify both epoch and time fields");
		if (oid == TIMESTAMPTZOID && (found_tz || found_gmtoff))
			luaL_error(L, "cannot specify timezone with epoch for timestamptz");
	}
	else if (oid == DATEOID || oid == TIMESTAMPTZOID || oid == TIMESTAMPOID)
	{
		if (!found_year)
			luaL_error(L, "missing datetime field '%s'", "year");
		if (!found_mon)
			luaL_error(L, "missing datetime field '%s'", "mon");
		if (!found_mday)
			luaL_error(L, "missing datetime field '%s'", "day");
		if (oid != TIMESTAMPTZOID && (found_tz || found_gmtoff))
			luaL_error(L, "cannot specify timezone for this type");
	}
	else if (oid == TIMEOID || oid == TIMETZOID)
	{
		if (!found_hour)
			luaL_error(L, "missing datetime field '%s'", "hour");
		if (found_sec && !found_min)
			luaL_error(L, "missing datetime field '%s'", "min");
		if (oid == TIMETZOID && found_tz && !found_gmtoff)
			luaL_error(L, "non-numeric timezones not supported for 'timetz'");
		if (oid != TIMETZOID && (found_tz || found_gmtoff))
			luaL_error(L, "cannot specify timezone for this type");
	}

	if (inf_sign && !(oid == TIMESTAMPOID || oid == TIMESTAMPTZOID))
		luaL_error(L, "infinite values not permitted for this type");

	d = pllua_newdatum(L, lua_upvalueindex(1), 0);

	PLLUA_TRY();
	{
		if (found_tz || found_gmtoff)
		{
			tz = (found_gmtoff ? pg_tzset_offset(-gmtoff) :
				  tzname ? pg_tzset(tzname) : session_timezone);
			if (!tz)
				ereport(ERROR,
						(errmsg("invalid timezone specified")));
		}

		if (found_epoch)
		{
			microsecs += epoch_microsecs;

			if (oid != DATEOID)
			{
				tmpflt = microsecs / 1000000.0;
				iresult = DirectFunctionCall7(make_interval,
											  Int32GetDatum(0),Int32GetDatum(0),Int32GetDatum(0),
											  Int32GetDatum(0),Int32GetDatum(0),Int32GetDatum(0),
											  Float8GetDatumFast(tmpflt));
			}

			switch (oid)
			{
				case TIMESTAMPTZOID:
				case TIMESTAMPOID:
					if (inf_sign != 0)
					{
						Timestamp tresult;
						if (inf_sign > 0)
							TIMESTAMP_NOEND(tresult);
						else
							TIMESTAMP_NOBEGIN(tresult);
						result = TimestampGetDatum(tresult);
						break;
					}

					tsval = time_t_to_timestamptz(0);
					result = DirectFunctionCall2(timestamptz_pl_interval,
												 TimestampTzGetDatum(tsval),
												 iresult);

					if (oid == TIMESTAMPOID &&
						((!found_gmtoff && found_tz) ||
						 (found_gmtoff && gmtoff != 0)))
					{
						fsec_t		fsec;
						int			tzo;
						Timestamp	newresult;

						if (timestamp2tm(DatumGetTimestampTz(result), &tzo, &tm, &fsec, NULL, tz) != 0)
							ereport(ERROR,
									(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
									 errmsg("timestamp out of range")));
						if (tm2timestamp(&tm, fsec, NULL, &newresult) != 0)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
									 errmsg("could not convert to time zone")));
						result = TimestampGetDatum(newresult);
					}
					break;

				case DATEOID:
					if ((!found_tz && !found_gmtoff) ||
						(found_gmtoff && gmtoff == 0))
					{
						int64 jdate = floordiv(microsecs, INT64CONST(86400) * 1000000) + UNIX_EPOCH_JDATE - POSTGRES_EPOCH_JDATE;
						if (!IS_VALID_DATE(jdate))
							ereport(ERROR,
									(errmsg("date value out of range")));
						dateresult = jdate;
					}
					else
					{
						struct pg_tm *tmp;
						pg_time_t tval = floordiv(microsecs, 1000000);
						tmp = pg_localtime(&tval, tz);
						if (!tmp)
							elog(ERROR, "date value conversion failed");
						dateresult = date2j(tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday) - POSTGRES_EPOCH_JDATE;
					}
					result = DateADTGetDatum(dateresult);
					break;

				case TIMEOID:
					{
						float8 secs = 0.0;
						result = DirectFunctionCall3(make_time,
													 Int32GetDatum(0),
													 Int32GetDatum(0),
													 Float8GetDatumFast(secs));
						result = DirectFunctionCall2(time_pl_interval, result, iresult);
					}
					break;

				case TIMETZOID:
					{
						char strbuf[32];
						int64 absoff = gmtoff < 0 ? -gmtoff : gmtoff;
						Datum tmpdatum;
						sprintf(strbuf, "00:00:00%c%02d:%02d:%02d",
								gmtoff < 0 ? '-' : '+',
								(int) (absoff / 3600),
								(int) ((absoff / 60) % 60),
								(int) (absoff % 60));
						tmpdatum = DirectFunctionCall3(timetz_in,
													   CStringGetDatum(strbuf),
													   ObjectIdGetDatum(TIMETZOID),
													   Int32GetDatum(-1));
						result = DirectFunctionCall2(timetz_pl_interval, tmpdatum, iresult);
						pfree(DatumGetPointer(tmpdatum));
					}
					break;

				case INTERVALOID:
					result = iresult;
					break;
			}
		}
		else
		{
			PGFunction addfunc = NULL;

			/*
			 * We have to normalize the pg_tm ourselves, except for interval
			 * and mday fields. But note that xx:59:60 is allowed, as is
			 * 24:00:00.
			 *
			 * The semantics of overflowing from minutes to hours to days here
			 * are highly questionable at best, but implementations of POSIX
			 * mktime seem to do it this way too.
			 */
			if (oid != INTERVALOID)
			{
				if (tm.tm_hour != 24 || tm.tm_min != 0 || tm.tm_sec != 0)
				{
					if (tm.tm_sec < 0 || tm.tm_sec > 60 ||
						(tm.tm_sec == 60 && tm.tm_min != 59))
						tm.tm_sec = calc_overflow(tm.tm_sec, 60, &tm.tm_min);
					if (tm.tm_min < 0 || tm.tm_min >= 60)
						tm.tm_min = calc_overflow(tm.tm_min, 60, &tm.tm_hour);
					if (tm.tm_hour < 0 || tm.tm_hour >= 24)
						tm.tm_hour = calc_overflow(tm.tm_hour, 24, &tm.tm_mday);
				}
				if (tm.tm_mon < 1 || tm.tm_mon > 12)
					tm.tm_mon = 1 + calc_overflow(tm.tm_mon - 1, 12, &tm.tm_year);
			}

			switch (oid)
			{
				case DATEOID:
					dateresult = date2j(tm.tm_year, tm.tm_mon, tm.tm_mday) - POSTGRES_EPOCH_JDATE;
					result = DateADTGetDatum(dateresult);
					break;

				case TIMESTAMPTZOID:
					{
						TimestampTz newresult;
						int tzo = determine_timezone_offset(&tm, tz ? tz : session_timezone);
						if (tm2timestamp(&tm, 0, &tzo, &newresult) != 0)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
									 errmsg("could not convert to timestamp")));
						result = TimestampTzGetDatum(newresult);
						addfunc = timestamptz_pl_interval;
					}
					break;

				case TIMESTAMPOID:
					{
						TimestampTz newresult;
						if (tm2timestamp(&tm, 0, NULL, &newresult) != 0)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
									 errmsg("could not convert to timestamp")));
						result = TimestampGetDatum(newresult);
						addfunc = timestamp_pl_interval;
					}
					break;

				case TIMEOID:
					{
						float8 secs = tm.tm_sec;
						result = DirectFunctionCall3(make_time,
													 Int32GetDatum(tm.tm_hour),
													 Int32GetDatum(tm.tm_min),
													 Float8GetDatumFast(secs));
						addfunc = time_pl_interval;
					}
					break;

				case TIMETZOID:
					{
						char strbuf[32];
						int64 absoff = gmtoff < 0 ? -gmtoff : gmtoff;
						sprintf(strbuf, "%02d:%02d:%02d%c%02d:%02d:%02d",
								tm.tm_hour, tm.tm_min, tm.tm_sec,
								gmtoff < 0 ? '-' : '+',
								(int) (absoff / 3600),
								(int) ((absoff / 60) % 60),
								(int) (absoff % 60));
						result = DirectFunctionCall3(timetz_in,
													 CStringGetDatum(strbuf),
													 ObjectIdGetDatum(TIMETZOID),
													 Int32GetDatum(-1));
						addfunc = timetz_pl_interval;
					}
					break;

				case INTERVALOID:
					{
						float8 secs = tm.tm_sec + (microsecs / 1000000.0);
						result = DirectFunctionCall7(make_interval,
													 Int32GetDatum(tm.tm_year),
													 Int32GetDatum(tm.tm_mon),
													 Int32GetDatum(0), /* weeks */
													 Int32GetDatum(tm.tm_mday),
													 Int32GetDatum(tm.tm_hour),
													 Int32GetDatum(tm.tm_min),
													 Float8GetDatumFast(secs));
					}
					break;
			}

			if (microsecs != 0 && oid != DATEOID && oid != INTERVALOID)
			{
				float8 secs = microsecs / 1000000.0;
				iresult = DirectFunctionCall7(make_interval,
											  Int32GetDatum(0),Int32GetDatum(0),Int32GetDatum(0),
											  Int32GetDatum(0),Int32GetDatum(0),Int32GetDatum(0),
											  Float8GetDatumFast(secs));
				result = DirectFunctionCall2(addfunc, result, iresult);
			}
		}

		d->value = result;
		pllua_savedatum(L, d, t);
	}
	PLLUA_CATCH_RETHROW();

	return 1;
}


static float8
pllua_time_raw_part(lua_State *L, const char *part, Datum val, Oid oid, PGFunction func, bool *isnull)
{
	volatile float8 res = 0;

	*isnull = false;

	PLLUA_TRY();
	{
		text *part_text = cstring_to_text(part);
		Datum resd;
		LOCAL_FCINFO(fcinfo, 2);

		if (oid == DATEOID)
			val = DirectFunctionCall1(date_timestamp, val);

		InitFunctionCallInfoData(*fcinfo, NULL, 2, InvalidOid, NULL, NULL);

		LFCI_ARG_VALUE(fcinfo,0) = PointerGetDatum(part_text);
		LFCI_ARG_VALUE(fcinfo,1) = val;
		LFCI_ARGISNULL(fcinfo,0) = false;
		LFCI_ARGISNULL(fcinfo,1) = false;

		resd = (*func) (fcinfo);

		if (fcinfo->isnull)
			*isnull = true;
		else
			res = DatumGetFloat8(resd);
	}
	PLLUA_CATCH_RETHROW();

	return res;
}


static int
pllua_time_as_table(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	Oid oid = (Oid) lua_tointeger(L, lua_upvalueindex(2));
	static struct pg_tm ztm = { 0 };
	struct pg_tm tm;
	fsec_t fsec = 0;
	float8 epoch_flt = 0;
	Datum val = d->value;
	float8 tmpflt;
	double tmpflt2;
	int tmpint;
	int64 microsecs = 0;
	bool isnull;
	const char *tzname = NULL;
	const char *tzn = NULL;
	int tzo = 0;
	pg_tz *tz = NULL;
	bool omit_date = false;
	bool omit_time = false;
	int found_tz = 0;
	int found_gmtoff = 0;
	int64 gmtoff = 0;

	tm = ztm;
	tm.tm_isdst = -1;

	lua_settop(L, 2);

	if (oid == TIMESTAMPTZOID)
	{
		switch (lua_type(L, 2))
		{
			case LUA_TNIL:
			case LUA_TNONE:
			case LUA_TBOOLEAN:
				break;

			case LUA_TSTRING:
				{
					int tzoff = 0;
					found_tz = 1;
					tzname = lua_tostring(L, -1);
					if (tzname && DecodeTimezone((char *) tzname, &tzoff) == 0)
					{
						gmtoff = -tzoff;
						found_gmtoff = 1;
					}
				}
				break;

			default:
				getnumber(L, 2, &gmtoff, NULL, NULL, "timezone");
				found_gmtoff = 1;
				break;
		}
	}
	else if (!lua_isnil(L, 2))
		luaL_error(L, "cannot specify timezone parameter for this type");

	switch (oid)
	{
		case DATEOID:
			{
				DateADT dval = DatumGetDateADT(val);
				j2date(dval + POSTGRES_EPOCH_JDATE,
					   &tm.tm_year, &tm.tm_mon, &tm.tm_mday);
				omit_time = true;
			}
			break;

		case TIMESTAMPTZOID:
		case TIMESTAMPOID:
			/*
			 * We abuse the fact that these two have the same underlying
			 * representation.
			 */
			{
				Timestamp tstmp = DatumGetTimestamp(val);

				if (TIMESTAMP_NOT_FINITE(tstmp))
				{
					if (TIMESTAMP_IS_NOBEGIN(tstmp))
						epoch_flt = -get_float8_infinity();
					else
						epoch_flt = get_float8_infinity();
					break;
				}
				PLLUA_TRY();
				{
					if (found_tz || found_gmtoff)
					{
						tz = (found_gmtoff ? pg_tzset_offset(-gmtoff) :
							  tzname ? pg_tzset(tzname) : session_timezone);
						if (!tz)
							ereport(ERROR,
									(errmsg("invalid timezone specified")));
					}

					if (timestamp2tm(tstmp,
									  (oid == TIMESTAMPTZOID) ? &tzo : NULL,
									  &tm,
									  &fsec,
									  (oid == TIMESTAMPTZOID) ? &tzn : NULL,
									  tz) != 0)
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("timestamp out of range")));
				}
				PLLUA_CATCH_RETHROW();

				microsecs = FSEC_T_SCALE(fsec);
			}
			break;

		case TIMETZOID:
			tmpflt = pllua_time_raw_part(L, "timezone", val, oid, timetz_part, &isnull);
			if (isnull)
				luaL_error(L, "unexpected null from time_part");
			tm.tm_gmtoff = (int) tmpflt;
			FALLTHROUGH; /*FALLTHROUGH*/
		case TIMEOID:
			tmpflt = pllua_time_raw_part(L, "epoch", val, oid,
										 (oid == TIMEOID) ? time_part : timetz_part,
										 &isnull);
			if (isnull)
				luaL_error(L, "unexpected null from time_part");
			tmpflt += tm.tm_gmtoff;
			microsecs = lrint(1000000.0 * modf(tmpflt, &tmpflt2));
			tmpint = (int) tmpflt2;
			tm.tm_sec = (tmpint % 60);
			tm.tm_min = ((tmpint / 60) % 60);
			tm.tm_hour = (tmpint / 3600);
			omit_date = true;
			break;

		case INTERVALOID:
			{
#if PG_VERSION_NUM >= 150000
				struct pg_itm	itm = { 0 };
#endif
				Interval *itmp = DatumGetIntervalP(val);
				PLLUA_TRY();
				{
#if PG_VERSION_NUM >= 150000
					/* PG15 removes interval2tm, use interval2itm and copy the result to tm */
					interval2itm(*itmp, &itm);
					tm.tm_sec  = itm.tm_sec;
					tm.tm_min  = itm.tm_min;
					tm.tm_hour = itm.tm_hour;
					tm.tm_mday = itm.tm_mday;
					tm.tm_mon  = itm.tm_mon;
					tm.tm_year = itm.tm_year;
#else
					if (interval2tm(*itmp, &tm, &fsec) != 0)
						elog(ERROR, "interval output failed");
#endif
				}
				PLLUA_CATCH_RETHROW();

#if PG_VERSION_NUM >= 150000
				microsecs = FSEC_T_SCALE(itm.tm_usec);
#else
				microsecs = FSEC_T_SCALE(fsec);
#endif
			}
			break;
	}

	lua_createtable(L, 0, 10);

	if (epoch_flt != 0.0)
	{
		lua_pushnumber(L, epoch_flt);
		lua_setfield(L, -2, "epoch");
	}
	else
	{
		if (!omit_date)
		{
			lua_pushinteger(L, tm.tm_year);
			lua_setfield(L, -2, "year");
			lua_pushinteger(L, tm.tm_mon);
			lua_setfield(L, -2, "month");
			lua_pushinteger(L, tm.tm_mday);
			lua_setfield(L, -2, "day");
		}
		if (!omit_time)
		{
			lua_pushinteger(L, tm.tm_hour);
			lua_setfield(L, -2, "hour");
			lua_pushinteger(L, tm.tm_min);
			lua_setfield(L, -2, "min");
			lua_pushinteger(L, tm.tm_sec);
			lua_setfield(L, -2, "sec");
			lua_pushinteger(L, microsecs);
			lua_setfield(L, -2, "usec");
		}
		if (oid == TIMESTAMPTZOID && tm.tm_isdst >= 0)
		{
			lua_pushboolean(L, tm.tm_isdst != 0);
			lua_setfield(L, -2, "isdst");
		}
		if (oid == TIMESTAMPTZOID || oid == TIMETZOID)
		{
			lua_pushinteger(L, tm.tm_gmtoff);
			lua_setfield(L, -2, "timezone");
		}
		if (tzn)
		{
			lua_pushstring(L, tzn);
			lua_setfield(L, -2, "timezone_abbrev");
		}
	}

	return 1;
}


static int
pllua_time_part(lua_State *L, pllua_datum *d, Oid oid, const char *opart)
{
	const char *part = opart;
	PGFunction func;
	float8 res = 0;
	bool isnull = false;

	if (strcmp(opart,"epoch_msec") == 0 ||
		strcmp(opart,"epoch_usec") == 0)
		part = "epoch";
	if (strcmp(opart,"isoweek") == 0)
		part = "week";

	switch (oid)
	{
		case DATEOID:			func = timestamp_part; break;
		case TIMESTAMPTZOID:	func = timestamptz_part; break;
		case TIMESTAMPOID:		func = timestamp_part; break;
		case TIMEOID:			func = time_part; break;
		case TIMETZOID:			func = timetz_part; break;
		case INTERVALOID:		func = interval_part; break;
		default:
			luaL_error(L, "unknown datetime type");
	}

	res = pllua_time_raw_part(L, part, d->value, oid, func, &isnull);

	if (isnull)
		lua_pushnil(L);
	else if (isinf(res))
		lua_pushnumber(L, res);
	else if (part != opart)
	{
		if (strcmp(opart, "epoch_msec") == 0)
			lua_pushnumber(L, res * 1000.0);
		else if (strcmp(opart, "epoch_usec") == 0)
		{
#ifdef PLLUA_INT8_OK
			lua_pushinteger(L, (int64) llrint(res * 1000000.0));
#else
			lua_pushnumber(L, rint(res * 1000000.0));
#endif
		}
		else
			lua_pushinteger(L, (lua_Integer) lrint(res));
	}
	else if (strcmp(part,"epoch") == 0 || strcmp(part,"second") == 0)
		lua_pushnumber(L, res);
	else
		lua_pushinteger(L, (lua_Integer) lrint(res));

	return 1;
}


static int
pllua_time_index(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	Oid oid = (Oid) lua_tointeger(L, lua_upvalueindex(2));
	const char *part = luaL_checkstring(L, 2);

	lua_settop(L, 2);

	if (lua_getfield(L, lua_upvalueindex(3), part) != LUA_TNIL)
		return 1;
	lua_pop(L, 1);
	return pllua_time_part(L, d, oid, part);
}


static luaL_Reg time_methods[] = {
	{ "as_table", pllua_time_as_table },
	{ NULL, NULL }
};

static luaL_Reg time_meta[] = {
	{ "tosql", pllua_time_tosql },
	{ "__index", pllua_time_index },
	{ NULL, NULL }
};

static luaL_Reg time_funcs[] = {
	{ NULL, NULL }
};

int pllua_open_time(lua_State *L)
{
	static Oid oidlist[] = { TIMESTAMPTZOID, TIMESTAMPOID,
							 DATEOID, TIMEOID, TIMETZOID,
							 INTERVALOID,
							 InvalidOid };
	int i;

	lua_settop(L, 0);

	lua_newtable(L);  /* module table at index 1 */
	luaL_setfuncs(L, time_funcs, 0);

	for (i = 0; OidIsValid(oidlist[i]); ++i)
	{
		Oid oid = oidlist[i];
		lua_pushcfunction(L, pllua_typeinfo_lookup);
		lua_pushinteger(L, oid);
		lua_call(L, 1, 1);
		lua_getuservalue(L, -1);

		/* first upvalue for metamethods */
		lua_pushvalue(L, -2);
		/* second upvalue for metamethods */
		lua_pushinteger(L, oid);

		/* methods table */
		lua_newtable(L);
		/* first upvalue for methods */
		lua_pushvalue(L, -3);
		/* second upvalue for methods */
		lua_pushinteger(L, oid);
		/* methods table is third upvalue for metamethods */
		luaL_setfuncs(L, time_methods, 2);

		luaL_setfuncs(L, time_meta, 3);
		lua_pop(L, 2);
	}

	lua_settop(L, 1);
	return 1;
}
