/*
 * This file is part of Firestorm NIDS.
 * Copyright (c) 2008 Gianni Tedesco <gianni@scaramanga.co.uk>
 * Released under the terms of the GNU GPL version 3
*/
#ifndef _FIRESTORM_TIME_HEADER_INCLUDED_
#define _FIRESTORM_TIME_HEADER_INCLUDED_

#if HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#if TIME_WITH_SYS_TIME
#include <time.h>
#endif

typedef uint32_t timestamp_t;

#define TIMESTAMP_INFINITE 0xffffffffU
#define TIMESTAMP_HZ 1

/** Get the current system time (usually you do not want to use this) */
timestamp_t time_gettime(void);

/** Get OS-specific virtual timestamp (cpu time) */
timestamp_t time_getvtime(void);

/* return 1 if t1 is before t2 (wrap safe) */
static inline int time_before(timestamp_t before, timestamp_t after)
{
	return (int32_t)(before - after) < 0;
}

/* return 1 if t1 is after t2 (wrap safe) */
static inline int time_after(timestamp_t after, timestamp_t before)
{
	return (int32_t)(before - after) < 0;
}

/** Calculate GCD of two timestamps */
timestamp_t time_gcd(timestamp_t n, timestamp_t d);

/* Various OS specific conversions */
static inline timestamp_t time_from_timeval(struct timeval *) _nonull(1);
static inline timestamp_t time_from_timeval(struct timeval *tv)
{
	return tv->tv_sec;
}

time_t time_to_time_t(timestamp_t);
void time_to_local(timestamp_t, struct tm *) _nonull(2);
void time_to_gmt(timestamp_t, struct tm *) _nonull(2);
void time_to_timeval(timestamp_t, struct timeval *) _nonull(2);

#endif /* _FIRESTORM_TIME_HEADER_INCLUDED_ */
