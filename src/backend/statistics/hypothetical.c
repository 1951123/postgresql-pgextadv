/* Backend-local hypothetical extended-statistics overlay. */
#include "postgres.h"
#include "catalog/pg_statistic_ext.h"
#include "fmgr.h"
#include "access/htup_details.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/hypothetical.h"
#include "utils/array.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

typedef struct HypoPayloadKey { Oid statoid; char kind; } HypoPayloadKey;
typedef struct HypoPayloadEntry
{ HypoPayloadKey key; Oid relid; bytea *payload; bool absent_native; } HypoPayloadEntry;
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

static bool
repository_oid(Oid statoid, Oid *relid)
{
	HypoPayloadKey key;
	HypoPayloadEntry *entry;
	if (HypoRepository == NULL)
		return false;
	MemSet(&key, 0, sizeof(key));
	key.statoid = statoid;
	key.kind = HYPOTHETICAL_EXTSTATS_MCV;
	entry = hash_search(HypoRepository, &key, HASH_FIND, NULL);
	if (entry == NULL)
	{
		key.kind = HYPOTHETICAL_EXTSTATS_DEPENDENCIES;
		entry = hash_search(HypoRepository, &key, HASH_FIND, NULL);
	}
	if (entry == NULL)
		return false;
	*relid = entry->relid;
	return true;
}

bool
hypothetical_extstats_absent_native(Oid statoid, HypotheticalExtStatsKind kind)
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
	return entry->absent_native;
}

bool
hypothetical_extstats_registered(Oid statoid, HypotheticalExtStatsKind kind)
{
	HypoPayloadKey key;

	if (!HypoEnabled || HypoActive == NULL || HypoRepository == NULL ||
		hash_search(HypoActive, &statoid, HASH_FIND, NULL) == NULL)
		return false;
	MemSet(&key, 0, sizeof(key));
	key.statoid = statoid;
	key.kind = kind;
	{
		HypoPayloadEntry *entry = hash_search(HypoRepository, &key, HASH_FIND, NULL);
		return entry != NULL && !entry->absent_native;
	}
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
	entry->absent_native = false;
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
	entry->absent_native = true;
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
		Oid relid;
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
		Oid relid;
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
	int i;
	bool targeted = false;
	if (!HypoEnabled)
		return catalog_oids;
	foreach(lc, catalog_oids)
	{
		Oid registered_relid;
		if (repository_oid(lfirst_oid(lc), &registered_relid) && registered_relid == relid)
			targeted = true;
	}
	if (!targeted)
		return catalog_oids;
	for (i = 0; i < HypoOrderLength; i++)
	{
		HypoActiveEntry *active = hash_search(HypoActive, &HypoOrder[i], HASH_FIND, NULL);
		if (active->relid == relid && list_member_oid(catalog_oids, HypoOrder[i]))
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
