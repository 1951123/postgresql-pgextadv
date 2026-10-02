/*-------------------------------------------------------------------------
 *
 * analyze_sample_cache.h
 *
 * Internal export/import support for ANALYZE acquisition samples.
 *
 *-------------------------------------------------------------------------
 */
#ifndef ANALYZE_SAMPLE_CACHE_H
#define ANALYZE_SAMPLE_CACHE_H

#include "access/htup.h"
#include "access/relation.h"

/* The cache contains sampled tuples, never pg_statistic payloads. */
typedef struct SampleCache
{
	uint32		format_version;
	Oid			relation_oid;
	char	   *relation_name;
	int			natts;
	uint32		tuple_count;
	double		totalrows;
	HeapTuple *tuples;
} SampleCache;

extern void sample_cache_export(Relation relation, const char *path,
							HeapTuple *tuples, int tuple_count,
							double totalrows);
extern SampleCache *sample_cache_import(Relation relation, const char *path,
									int targrows);
extern void sample_cache_free(SampleCache *cache, bool free_tuples);

#endif						/* ANALYZE_SAMPLE_CACHE_H */
