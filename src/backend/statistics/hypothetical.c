/* Backend-local hypothetical extended-statistics overlay. */
#include "postgres.h"
#include "catalog/pg_statistic_ext.h"
#include "common/hashfn.h"
#include "fmgr.h"
#include "access/htup_details.h"
#include "nodes/bitmapset.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/hypothetical.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

typedef struct HypoPayloadKey { Oid statoid; char kind; } HypoPayloadKey;
typedef struct HypoPayloadEntry
{
	HypoPayloadKey key;
	Oid relid;
	bytea *payload;
	Bitmapset *keys;
	char *candidate_id;
	bool absent_native;
	bool virtual_definition;
} HypoPayloadEntry;
typedef struct HypoActiveEntry { Oid statoid; Oid relid; } HypoActiveEntry;

static MemoryContext HypoContext;
static MemoryContext HypoRepositoryContext;
static MemoryContext HypoActiveContext;
static HTAB *HypoRepository;
static HTAB *HypoActive;
static Oid *HypoOrder;
static int HypoOrderLength;
static bool HypoEnabled;

static void
hypothetical_extstats_init(void)
{
	HASHCTL ctl;
	if (HypoContext != NULL)
		return;
	HypoContext = AllocSetContextCreate(TopMemoryContext,
			"HypotheticalExtStatsContext", ALLOCSET_DEFAULT_SIZES);
	HypoRepositoryContext = AllocSetContextCreate(HypoContext,
			"HypotheticalExtStatsRepository", ALLOCSET_DEFAULT_SIZES);
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(HypoPayloadKey);
	ctl.entrysize = sizeof(HypoPayloadEntry);
	ctl.hcxt = HypoRepositoryContext;
	HypoRepository = hash_create("hypothetical extstats payloads", 32, &ctl,
			HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

void
hypothetical_extstats_reset(void)
{
	if (HypoContext != NULL)
		MemoryContextDelete(HypoContext);
	HypoContext = HypoRepositoryContext = HypoActiveContext = NULL;
	HypoRepository = HypoActive = NULL;
	HypoOrder = NULL;
	HypoOrderLength = 0;
	HypoEnabled = false;
}

static HypoPayloadEntry *
repository_entry(Oid statoid, HypotheticalExtStatsKind kind)
{
	HypoPayloadKey key;

	if (HypoRepository == NULL)
		return NULL;
	MemSet(&key, 0, sizeof(key));
	key.statoid = statoid;
	key.kind = kind;
	return hash_search(HypoRepository, &key, HASH_FIND, NULL);
}

static HypoPayloadEntry *
repository_entry_any(Oid statoid)
{
	HypoPayloadEntry *entry;

	entry = repository_entry(statoid, HYPOTHETICAL_EXTSTATS_MCV);
	if (entry == NULL)
		entry = repository_entry(statoid, HYPOTHETICAL_EXTSTATS_DEPENDENCIES);
	return entry;
}

static bool
active_oid(Oid statoid)
{
	return HypoEnabled && HypoActive != NULL &&
		hash_search(HypoActive, &statoid, HASH_FIND, NULL) != NULL;
}

static bool
repository_oid(Oid statoid, Oid *relid)
{
	HypoPayloadEntry *entry = repository_entry_any(statoid);
	if (entry == NULL)
		return false;
	*relid = entry->relid;
	return true;
}

bool
hypothetical_extstats_absent_native(Oid statoid, HypotheticalExtStatsKind kind)
{
	HypoPayloadEntry *entry;
	if (!active_oid(statoid))
		return false;
	entry = repository_entry(statoid, kind);
	if (entry == NULL)
		ereport(ERROR, (errmsg("active statistics object %u has no registered payload for kind %c",
			statoid, kind)));
	return entry->absent_native;
}

bool
hypothetical_extstats_registered(Oid statoid, HypotheticalExtStatsKind kind)
{
	HypoPayloadEntry *entry;

	if (!active_oid(statoid))
		return false;
	entry = repository_entry(statoid, kind);
	return entry != NULL && !entry->absent_native;
}

bool
hypothetical_extstats_definition(Oid statoid, Oid relid, char *kind,
								 Bitmapset **keys)
{
	HypoPayloadEntry *entry = repository_entry_any(statoid);

	if (entry == NULL || !entry->virtual_definition || entry->relid != relid ||
		!active_oid(statoid) || entry->absent_native)
		return false;
	*kind = entry->key.kind;
	*keys = bms_copy(entry->keys);
	return true;
}

static void
validate_kind(HypotheticalExtStatsKind kind)
{
	if (kind != HYPOTHETICAL_EXTSTATS_MCV &&
		kind != HYPOTHETICAL_EXTSTATS_DEPENDENCIES)
		ereport(ERROR, (errmsg("unsupported hypothetical statistics kind: %c", kind)));
}

static Oid
allocate_virtual_oid(const char *candidate_id, Oid relid,
					 HypotheticalExtStatsKind kind)
{
	char identity[256];
	uint32 hash;
	Oid statoid;

	snprintf(identity, sizeof(identity), "%u:%c:%s", relid, kind, candidate_id);
	hash = DatumGetUInt32(hash_any((const unsigned char *) identity,
									 strlen(identity)));
	/* OID zero is InvalidOid; the high bit marks backend-local values. */
	statoid = (Oid) (hash | 0x80000000U);
	if (!OidIsValid(statoid))
		statoid = FirstNormalObjectId;

	for (;; statoid++)
	{
		HeapTuple tuple;

		if (statoid == InvalidOid)
			continue;
		if (repository_entry_any(statoid) != NULL)
			continue;
		/* This lookup is collision hygiene only.  Planner resolution of a
		 * virtual identity never consults pg_statistic_ext. */
		tuple = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statoid));
		if (!HeapTupleIsValid(tuple))
			return statoid;
		ReleaseSysCache(tuple);
	}
}

static Bitmapset *
keys_from_array(ArrayType *array)
{
	Datum *datums;
	bool *nulls;
	int count;
	Bitmapset *keys = NULL;
	int i;

	deconstruct_array_builtin(array, INT2OID, &datums, &nulls, &count);
	if (count < 2 || count > STATS_MAX_DIMENSIONS)
		ereport(ERROR, (errmsg("hypothetical statistics require 2..%d attribute keys",
						  STATS_MAX_DIMENSIONS)));
	for (i = 0; i < count; i++)
	{
		AttrNumber attnum;

		if (nulls[i])
			ereport(ERROR, (errmsg("hypothetical attribute keys cannot contain NULL")));
		attnum = DatumGetInt16(datums[i]);
		if (attnum <= 0 || bms_is_member(attnum, keys))
			ereport(ERROR, (errmsg("hypothetical attribute keys must be positive and unique")));
		keys = bms_add_member(keys, attnum);
	}
	pfree(datums);
	pfree(nulls);
	return keys;
}

static Oid
register_definition_internal(const char *candidate_id, Oid relid,
							 HypotheticalExtStatsKind kind, Bitmapset *keys,
							 const bytea *payload, bool absent_native)
{
	HypoPayloadKey key;
	HypoPayloadEntry *entry;
	MemoryContext oldcontext;
	bool found;
	Oid statoid;
	void *decoded = NULL;

	if (candidate_id == NULL || candidate_id[0] == '\0' || !OidIsValid(relid))
		ereport(ERROR, (errmsg("invalid catalogless hypothetical definition")));
	validate_kind(kind);
	if (keys == NULL || bms_num_members(keys) < 2)
		ereport(ERROR, (errmsg("catalogless hypothetical definition has no attribute keys")));
	if (!absent_native)
	{
		if (payload == NULL || VARSIZE_ANY_EXHDR(payload) == 0)
			ereport(ERROR, (errmsg("empty hypothetical statistics payload")));
		if (kind == HYPOTHETICAL_EXTSTATS_MCV)
			decoded = statext_mcv_deserialize((bytea *) payload);
		else
			decoded = statext_dependencies_deserialize((bytea *) payload);
		pfree(decoded);
	}
	hypothetical_extstats_init();
	statoid = allocate_virtual_oid(candidate_id, relid, kind);
	MemSet(&key, 0, sizeof(key));
	key.statoid = statoid;
	key.kind = kind;
	entry = hash_search(HypoRepository, &key, HASH_ENTER, &found);
	Assert(!found);
	oldcontext = MemoryContextSwitchTo(HypoRepositoryContext);
	entry->relid = relid;
	entry->keys = bms_copy(keys);
	entry->candidate_id = pstrdup(candidate_id);
	entry->payload = payload ? PG_DETOAST_DATUM_COPY(PointerGetDatum(payload)) : NULL;
	entry->absent_native = absent_native;
	entry->virtual_definition = true;
	MemoryContextSwitchTo(oldcontext);
	return statoid;
}

Oid
hypothetical_extstats_register_definition(const char *candidate_id, Oid relid,
							 HypotheticalExtStatsKind kind, Bitmapset *keys,
							 const bytea *payload)
{
	return register_definition_internal(candidate_id, relid, kind, keys, payload, false);
}

Oid
hypothetical_extstats_register_definition_absent(const char *candidate_id, Oid relid,
											 HypotheticalExtStatsKind kind, Bitmapset *keys)
{
	return register_definition_internal(candidate_id, relid, kind, keys, NULL, true);
}

void
hypothetical_extstats_register(Oid statoid, Oid relid,
		HypotheticalExtStatsKind kind, const bytea *payload)
{
	HypoPayloadKey key;
	HypoPayloadEntry *entry;
	HeapTuple tuple;
	Oid actual_relid;
	MemoryContext oldcontext;
	bool found;
	void *decoded;

	if (!OidIsValid(statoid) || !OidIsValid(relid))
		ereport(ERROR, (errmsg("invalid hypothetical statistics identity")));
	if (kind != HYPOTHETICAL_EXTSTATS_MCV &&
		kind != HYPOTHETICAL_EXTSTATS_DEPENDENCIES)
		ereport(ERROR, (errmsg("unsupported hypothetical statistics kind: %c", kind)));
	if (payload == NULL || VARSIZE_ANY_EXHDR(payload) == 0)
		ereport(ERROR, (errmsg("empty hypothetical statistics payload")));
	tuple = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statoid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR, (errmsg("statistics object %u does not exist", statoid)));
	actual_relid = ((Form_pg_statistic_ext) GETSTRUCT(tuple))->stxrelid;
	ReleaseSysCache(tuple);
	if (actual_relid != relid)
		ereport(ERROR, (errmsg("statistics object %u belongs to relation %u, not %u",
			statoid, actual_relid, relid)));
	if (kind == HYPOTHETICAL_EXTSTATS_MCV)
		decoded = statext_mcv_deserialize((bytea *) payload);
	else
		decoded = statext_dependencies_deserialize((bytea *) payload);
	pfree(decoded);

	hypothetical_extstats_init();
	MemSet(&key, 0, sizeof(key));
	key.statoid = statoid;
	key.kind = kind;
	entry = hash_search(HypoRepository, &key, HASH_ENTER, &found);
	if (found)
		ereport(ERROR, (errmsg("hypothetical payload for statistics object %u kind %c is already registered",
			statoid, kind)));
	oldcontext = MemoryContextSwitchTo(HypoRepositoryContext);
	entry->relid = relid;
	entry->payload = PG_DETOAST_DATUM_COPY(PointerGetDatum(payload));
	entry->keys = NULL;
	entry->candidate_id = NULL;
	entry->absent_native = false;
	entry->virtual_definition = false;
	MemoryContextSwitchTo(oldcontext);
}

void
hypothetical_extstats_register_absent(Oid statoid, Oid relid,
		HypotheticalExtStatsKind kind)
{
	HypoPayloadKey key;
	HypoPayloadEntry *entry;
	HeapTuple tuple;
	Oid actual_relid;
	bool found;

	if (!OidIsValid(statoid) || !OidIsValid(relid))
		ereport(ERROR, (errmsg("invalid hypothetical statistics identity")));
	if (kind != HYPOTHETICAL_EXTSTATS_MCV &&
		kind != HYPOTHETICAL_EXTSTATS_DEPENDENCIES)
		ereport(ERROR, (errmsg("unsupported hypothetical statistics kind: %c", kind)));
	tuple = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statoid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR, (errmsg("statistics object %u does not exist", statoid)));
	actual_relid = ((Form_pg_statistic_ext) GETSTRUCT(tuple))->stxrelid;
	ReleaseSysCache(tuple);
	if (actual_relid != relid)
		ereport(ERROR, (errmsg("statistics object %u belongs to relation %u, not %u",
			statoid, actual_relid, relid)));
	hypothetical_extstats_init();
	MemSet(&key, 0, sizeof(key));
	key.statoid = statoid;
	key.kind = kind;
	entry = hash_search(HypoRepository, &key, HASH_ENTER, &found);
	if (found)
		ereport(ERROR, (errmsg("hypothetical payload for statistics object %u kind %c is already registered",
			statoid, kind)));
	entry->relid = relid;
	entry->payload = NULL;
	entry->keys = NULL;
	entry->candidate_id = NULL;
	entry->absent_native = true;
	entry->virtual_definition = false;
}

void
hypothetical_extstats_activate(List *ordered_oids)
{
	MemoryContext newcontext;
	MemoryContext oldcontext;
	HTAB *newactive;
	HASHCTL ctl;
	Oid *neworder;
	ListCell *lc;
	int index = 0;

	hypothetical_extstats_init();
	/* Validate before allocating replacement state, so activation is atomic. */
	foreach(lc, ordered_oids)
	{
		Oid oid = lfirst_oid(lc);
		Oid relid = InvalidOid;
		ListCell *prior;
		if (!repository_oid(oid, &relid))
			ereport(ERROR, (errmsg("cannot activate unregistered statistics object %u", oid)));
		for_each_cell(prior, ordered_oids, list_head(ordered_oids))
		{
			if (prior == lc)
				break;
			if (lfirst_oid(prior) == oid)
				ereport(ERROR, (errmsg("duplicate active statistics object %u", oid)));
		}
	}
	newcontext = AllocSetContextCreate(HypoContext,
			"HypotheticalExtStatsActiveDesign", ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(newcontext);
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(HypoActiveEntry);
	ctl.hcxt = newcontext;
	newactive = hash_create("active hypothetical extstats", 32, &ctl,
			HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	neworder = palloc(sizeof(Oid) * Max(1, list_length(ordered_oids)));
	foreach(lc, ordered_oids)
	{
		Oid oid = lfirst_oid(lc);
		Oid relid = InvalidOid;
		HypoActiveEntry *active;
		bool found;
		(void) repository_oid(oid, &relid);
		active = hash_search(newactive, &oid, HASH_ENTER, &found);
		Assert(!found);
		active->relid = relid;
		neworder[index++] = oid;
	}
	MemoryContextSwitchTo(oldcontext);
	if (HypoActiveContext != NULL)
		MemoryContextDelete(HypoActiveContext);
	HypoActiveContext = newcontext;
	HypoActive = newactive;
	HypoOrder = neworder;
	HypoOrderLength = index;
	HypoEnabled = true;
}

List *
hypothetical_extstats_filter(Oid relid, List *catalog_oids)
{
	List *result = NIL;
	ListCell *lc;
	HASH_SEQ_STATUS status;
	HypoPayloadEntry *entry;
	int i;
	bool targeted = false;
	if (!HypoEnabled)
		return catalog_oids;
	hash_seq_init(&status, HypoRepository);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		if (entry->relid == relid)
		{
			targeted = true;
			hash_seq_term(&status);
			break;
		}
	}
	if (!targeted)
		return catalog_oids;
	for (i = 0; i < HypoOrderLength; i++)
	{
		HypoActiveEntry *active = hash_search(HypoActive, &HypoOrder[i], HASH_FIND, NULL);
		HypoPayloadEntry *definition = repository_entry_any(HypoOrder[i]);
		if (active->relid == relid && definition != NULL &&
			!definition->absent_native &&
			(definition->virtual_definition || list_member_oid(catalog_oids, HypoOrder[i])))
			result = lappend_oid(result, HypoOrder[i]);
	}
	foreach(lc, catalog_oids)
	{
		Oid oid = lfirst_oid(lc);
		Oid registered_relid;
		if (!repository_oid(oid, &registered_relid))
			result = lappend_oid(result, oid);
	}
	list_free(catalog_oids);
	return result;
}

bool
hypothetical_extstats_payload(Oid statoid, HypotheticalExtStatsKind kind,
		const bytea **payload)
{
	HypoPayloadKey key;
	HypoPayloadEntry *entry;
	if (!HypoEnabled || HypoActive == NULL ||
		hash_search(HypoActive, &statoid, HASH_FIND, NULL) == NULL)
		return false;
	MemSet(&key, 0, sizeof(key));
	key.statoid = statoid;
	key.kind = kind;
	entry = hash_search(HypoRepository, &key, HASH_FIND, NULL);
	if (entry == NULL)
		ereport(ERROR, (errmsg("active statistics object %u has no registered payload for kind %c",
						  statoid, kind)));
	if (entry->absent_native)
		return false;
	*payload = entry->payload;
	return true;
}

Datum pg_hypothetical_extstats_reset(PG_FUNCTION_ARGS)
{ hypothetical_extstats_reset(); PG_RETURN_VOID(); }

Datum pg_hypothetical_extstats_register(PG_FUNCTION_ARGS)
{
	hypothetical_extstats_register(PG_GETARG_OID(0), PG_GETARG_OID(1),
			(HypotheticalExtStatsKind) PG_GETARG_CHAR(2), PG_GETARG_BYTEA_PP(3));
	PG_RETURN_VOID();
}

Datum pg_hypothetical_extstats_register_absent(PG_FUNCTION_ARGS)
{
	hypothetical_extstats_register_absent(PG_GETARG_OID(0), PG_GETARG_OID(1),
			(HypotheticalExtStatsKind) PG_GETARG_CHAR(2));
	PG_RETURN_VOID();
}

Datum pg_hypothetical_extstats_register_definition(PG_FUNCTION_ARGS)
{
	text *candidate = PG_GETARG_TEXT_PP(0);
	ArrayType *array = PG_GETARG_ARRAYTYPE_P(3);
	Bitmapset *keys = keys_from_array(array);
	Oid statoid = hypothetical_extstats_register_definition(
			text_to_cstring(candidate), PG_GETARG_OID(1),
			(HypotheticalExtStatsKind) PG_GETARG_CHAR(2), keys,
			PG_GETARG_BYTEA_PP(4));
	bms_free(keys);
	PG_RETURN_OID(statoid);
}

Datum pg_hypothetical_extstats_register_definition_absent(PG_FUNCTION_ARGS)
{
	text *candidate = PG_GETARG_TEXT_PP(0);
	ArrayType *array = PG_GETARG_ARRAYTYPE_P(3);
	Bitmapset *keys = keys_from_array(array);
	Oid statoid = hypothetical_extstats_register_definition_absent(
			text_to_cstring(candidate), PG_GETARG_OID(1),
			(HypotheticalExtStatsKind) PG_GETARG_CHAR(2), keys);
	bms_free(keys);
	PG_RETURN_OID(statoid);
}

Datum pg_hypothetical_extstats_activate(PG_FUNCTION_ARGS)
{
	ArrayType *array = PG_GETARG_ARRAYTYPE_P(0);
	Datum *datums;
	bool *nulls;
	int count;
	int i;
	List *oids = NIL;
	deconstruct_array_builtin(array, OIDOID, &datums, &nulls, &count);
	for (i = 0; i < count; i++)
	{
		if (nulls[i])
			ereport(ERROR, (errmsg("active hypothetical design cannot contain NULL")));
		oids = lappend_oid(oids, DatumGetObjectId(datums[i]));
	}
	hypothetical_extstats_activate(oids);
	list_free(oids);
	pfree(datums);
	pfree(nulls);
	PG_RETURN_VOID();
}

Datum pg_hypothetical_extstats_active(PG_FUNCTION_ARGS)
{
	Datum *datums;
	ArrayType *result;
	int i;
	if (!HypoEnabled)
		PG_RETURN_NULL();
	datums = palloc(sizeof(Datum) * Max(1, HypoOrderLength));
	for (i = 0; i < HypoOrderLength; i++)
		datums[i] = ObjectIdGetDatum(HypoOrder[i]);
	result = construct_array_builtin(datums, HypoOrderLength, OIDOID);
	pfree(datums);
	PG_RETURN_ARRAYTYPE_P(result);
}
