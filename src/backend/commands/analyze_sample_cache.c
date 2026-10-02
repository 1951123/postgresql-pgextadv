/*-------------------------------------------------------------------------
 *
 * analyze_sample_cache.c
 *		Binary export/import for ANALYZE acquisition samples.
 *
 * This is deliberately an internal, PostgreSQL-native format.  It is tied to
 * the server binary and relation tuple descriptor; it is not a data export
 * format and does not contain computed statistics.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdio.h>
#include <string.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "commands/analyze_sample_cache.h"
#include "miscadmin.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#define SAMPLE_CACHE_MAGIC "PGEXTSC1"
#define SAMPLE_CACHE_MAGIC_LEN 8
#define SAMPLE_CACHE_FORMAT_VERSION 1

static void sample_cache_write_bytes(FILE *file, const void *data, size_t len,
								 pg_crc32c *crc, const char *path);
static void sample_cache_read_bytes(FILE *file, void *data, size_t len,
								pg_crc32c *crc, const char *path);
static void sample_cache_write_u32(FILE *file, uint32 value, pg_crc32c *crc,
								  const char *path);
static uint32 sample_cache_read_u32(FILE *file, pg_crc32c *crc, const char *path);
static void sample_cache_write_string(FILE *file, const char *value,
								  pg_crc32c *crc, const char *path);
static char *sample_cache_read_string(FILE *file, pg_crc32c *crc,
								 const char *path);
static char *sample_cache_relation_identity(Relation relation);
static void sample_cache_check_descriptor(Relation relation, uint32 natts,
										 FILE *file,
										 pg_crc32c *crc, const char *path);
static void sample_cache_file_error(const char *path, const char *operation);

static void
sample_cache_file_error(const char *path, const char *operation)
{
	ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not %s ANALYZE sample cache file \"%s\": %m",
					 operation, path)));
}

static void
sample_cache_write_bytes(FILE *file, const void *data, size_t len,
							 pg_crc32c *crc, const char *path)
{
	if (len > 0 && fwrite(data, 1, len, file) != len)
		sample_cache_file_error(path, "write");
	if (len > 0)
		COMP_CRC32C(*crc, data, len);
}

static void
sample_cache_read_bytes(FILE *file, void *data, size_t len,
							pg_crc32c *crc, const char *path)
{
	if (len > 0 && fread(data, 1, len, file) != len)
		sample_cache_file_error(path, "read");
	if (len > 0)
		COMP_CRC32C(*crc, data, len);
}

static void
sample_cache_write_u32(FILE *file, uint32 value, pg_crc32c *crc,
							  const char *path)
{
	sample_cache_write_bytes(file, &value, sizeof(value), crc, path);
}

static uint32
sample_cache_read_u32(FILE *file, pg_crc32c *crc, const char *path)
{
	uint32	value;

	sample_cache_read_bytes(file, &value, sizeof(value), crc, path);
	return value;
}

static void
sample_cache_write_string(FILE *file, const char *value, pg_crc32c *crc,
							  const char *path)
{
	uint32	len = (uint32) strlen(value);

	sample_cache_write_u32(file, len, crc, path);
	sample_cache_write_bytes(file, value, len, crc, path);
}

static char *
sample_cache_read_string(FILE *file, pg_crc32c *crc, const char *path)
{
	uint32	len = sample_cache_read_u32(file, crc, path);
	char   *value;

	if (len > MaxAllocSize - 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("ANALYZE sample cache string is too large"),
				 errdetail("file \"%s\" contains a string of length %u",
						   path, len)));
	value = (char *) palloc(len + 1);
	sample_cache_read_bytes(file, value, len, crc, path);
	value[len] = '\0';
	return value;
}

/* Length-prefix both names so dots or other punctuation in identifiers are unambiguous. */
static char *
sample_cache_relation_identity(Relation relation)
{
	const char *namespace_name = get_namespace_name(RelationGetNamespace(relation));
	const char *relation_name = RelationGetRelationName(relation);

	return psprintf("%zu:%s%zu:%s",
					strlen(namespace_name), namespace_name,
					strlen(relation_name), relation_name);
}

static void
sample_cache_check_descriptor(Relation relation, uint32 natts, FILE *file,
								  pg_crc32c *crc,
								  const char *path)
{
	uint32	 i;

	if (relation->rd_att->natts != (int) natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("ANALYZE sample cache column count does not match relation"),
				 errdetail("cache has %u columns, relation has %d",
						   natts, relation->rd_att->natts)));

	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(relation->rd_att, i);
		Oid		typid = sample_cache_read_u32(file, crc, path);
		int32	typmod;
		Oid		collation;
		uint8	dropped;
		char   *attname;

		sample_cache_read_bytes(file, &typmod, sizeof(typmod), crc, path);
		collation = sample_cache_read_u32(file, crc, path);
		sample_cache_read_bytes(file, &dropped, sizeof(dropped), crc, path);
		attname = sample_cache_read_string(file, crc, path);

		if (attr->atttypid != typid || attr->atttypmod != typmod ||
			attr->attcollation != collation || attr->attisdropped != (dropped != 0) ||
			strcmp(NameStr(attr->attname), attname) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("ANALYZE sample cache column %u does not match relation",
							i + 1)));
		pfree(attname);
	}
}

void
sample_cache_export(Relation relation, const char *path, HeapTuple *tuples,
						int tuple_count, double totalrows)
{
	FILE	   *file;
	pg_crc32c	crc;
	uint32		i;
	uint32		natts = (uint32) relation->rd_att->natts;
	uint32		server_version = PG_VERSION_NUM;
	const char *server_version_string = PG_VERSION;
	char	   *relation_identity;

	file = AllocateFile(path, PG_BINARY_W);
	if (file == NULL)
		sample_cache_file_error(path, "open");

	INIT_CRC32C(crc);
	relation_identity = sample_cache_relation_identity(relation);
	sample_cache_write_bytes(file, SAMPLE_CACHE_MAGIC, SAMPLE_CACHE_MAGIC_LEN,
							 &crc, path);
	sample_cache_write_u32(file, SAMPLE_CACHE_FORMAT_VERSION, &crc, path);
	sample_cache_write_u32(file, server_version, &crc, path);
	sample_cache_write_string(file, server_version_string, &crc, path);
	sample_cache_write_u32(file, relation->rd_id, &crc, path);
	sample_cache_write_string(file, relation_identity, &crc, path);
	sample_cache_write_u32(file, natts, &crc, path);
	sample_cache_write_u32(file, (uint32) tuple_count, &crc, path);
	sample_cache_write_bytes(file, &totalrows, sizeof(totalrows), &crc, path);

	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(relation->rd_att, i);
		uint8	dropped = attr->attisdropped ? 1 : 0;

		sample_cache_write_u32(file, attr->atttypid, &crc, path);
		sample_cache_write_bytes(file, &attr->atttypmod, sizeof(attr->atttypmod),
							 &crc, path);
		sample_cache_write_u32(file, attr->attcollation, &crc, path);
		sample_cache_write_bytes(file, &dropped, sizeof(dropped), &crc, path);
		sample_cache_write_string(file, NameStr(attr->attname), &crc, path);
	}

	for (i = 0; i < (uint32) tuple_count; i++)
	{
		uint32	len = tuples[i]->t_len;

		if (len < sizeof(HeapTupleHeaderData) || len > MaxAllocSize)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid tuple length in ANALYZE sample")));
		sample_cache_write_u32(file, len, &crc, path);
		sample_cache_write_bytes(file, tuples[i]->t_data, len, &crc, path);
	}

	FIN_CRC32C(crc);
	if (fwrite(&crc, sizeof(crc), 1, file) != 1)
		sample_cache_file_error(path, "write");
	if (FreeFile(file) != 0)
		sample_cache_file_error(path, "close");
	pfree(relation_identity);
}

SampleCache *
sample_cache_import(Relation relation, const char *path, int targrows)
{
	SampleCache *cache;
	FILE	   *file;
	pg_crc32c	crc;
	pg_crc32c	stored_crc;
	char		magic[SAMPLE_CACHE_MAGIC_LEN];
	uint32		version;
	uint32		server_version;
	char	   *server_version_string;
	uint32		relation_oid;
	char	   *relation_name;
	uint32		natts;
	uint32		i;
	int		trailing;
	char	   *relation_identity;

	file = AllocateFile(path, PG_BINARY_R);
	if (file == NULL)
		sample_cache_file_error(path, "open");

	INIT_CRC32C(crc);
	sample_cache_read_bytes(file, magic, sizeof(magic), &crc, path);
	if (memcmp(magic, SAMPLE_CACHE_MAGIC, sizeof(magic)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid ANALYZE sample cache magic in \"%s\"", path)));

	version = sample_cache_read_u32(file, &crc, path);
	if (version != SAMPLE_CACHE_FORMAT_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unsupported ANALYZE sample cache version %u", version)));

	server_version = sample_cache_read_u32(file, &crc, path);
	server_version_string = sample_cache_read_string(file, &crc, path);
	if (server_version != PG_VERSION_NUM ||
		strcmp(server_version_string, PG_VERSION) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ANALYZE sample cache PostgreSQL version is incompatible"),
				 errdetail("cache is for %u (%s), server is %u (%s)",
						   server_version, server_version_string,
						   PG_VERSION_NUM, PG_VERSION)));
	pfree(server_version_string);

	relation_oid = sample_cache_read_u32(file, &crc, path);
	relation_name = sample_cache_read_string(file, &crc, path);
	relation_identity = sample_cache_relation_identity(relation);
	if (strcmp(relation_name, relation_identity) != 0 &&
		!(relation_oid == RelationGetRelid(relation) &&
		  strcmp(relation_name, RelationGetRelationName(relation)) == 0))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("ANALYZE sample cache relation identity does not match"),
				 errdetail("cache identity \"%s\" does not match relation \"%s\"",
						   relation_name, relation_identity)));
	pfree(relation_identity);

	natts = sample_cache_read_u32(file, &crc, path);
	cache = (SampleCache *) palloc0(sizeof(SampleCache));
	cache->format_version = version;
	cache->relation_oid = relation_oid;
	cache->relation_name = relation_name;
	cache->natts = (int) natts;
	cache->tuple_count = sample_cache_read_u32(file, &crc, path);
	sample_cache_read_bytes(file, &cache->totalrows, sizeof(cache->totalrows),
						&crc, path);

	if (cache->tuple_count > (uint32) targrows)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("ANALYZE sample cache row count is invalid"),
				 errdetail("cache has %u rows, ANALYZE requested at most %d",
						   cache->tuple_count, targrows)));

	sample_cache_check_descriptor(relation, natts, file, &crc, path);
	cache->tuples = (HeapTuple *) palloc(sizeof(HeapTuple) * cache->tuple_count);
	for (i = 0; i < cache->tuple_count; i++)
	{
		uint32		len = sample_cache_read_u32(file, &crc, path);
		HeapTuple	tuple;

		if (len < sizeof(HeapTupleHeaderData) || len > MaxAllocSize)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid tuple length in ANALYZE sample cache")));
		tuple = (HeapTuple) palloc(sizeof(HeapTupleData));
		tuple->t_len = len;
		ItemPointerSetInvalid(&tuple->t_self);
		tuple->t_tableOid = RelationGetRelid(relation);
		tuple->t_data = (HeapTupleHeader) palloc(len);
		sample_cache_read_bytes(file, tuple->t_data, len, &crc, path);
		cache->tuples[i] = tuple;
	}

	if (fread(&stored_crc, sizeof(stored_crc), 1, file) != 1)
		sample_cache_file_error(path, "read");
	FIN_CRC32C(crc);
	if (stored_crc != crc)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("checksum mismatch in ANALYZE sample cache \"%s\"",
						path)));
	trailing = fgetc(file);
	if (trailing != EOF)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("unexpected data after ANALYZE sample cache checksum"),
				 errdetail("file \"%s\" has trailing bytes", path)));
	if (ferror(file))
		sample_cache_file_error(path, "read");
	if (FreeFile(file) != 0)
		sample_cache_file_error(path, "close");

	return cache;
}

void
sample_cache_free(SampleCache *cache, bool free_tuples)
{
	uint32	i;

	if (cache == NULL)
		return;
	if (free_tuples && cache->tuples != NULL)
	{
		for (i = 0; i < cache->tuple_count; i++)
			heap_freetuple(cache->tuples[i]);
	}
	if (cache->tuples != NULL)
		pfree(cache->tuples);
	if (cache->relation_name != NULL)
		pfree(cache->relation_name);
	pfree(cache);
}
