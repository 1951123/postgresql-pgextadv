/*-------------------------------------------------------------------------
 *
 * hypothetical.h
 *    Backend-local hypothetical extended-statistics state.
 *
 *-------------------------------------------------------------------------
 */
#ifndef HYPOTHETICAL_H
#define HYPOTHETICAL_H

#include "postgres.h"

#include "nodes/pg_list.h"

typedef enum HypotheticalExtStatsKind
{
	HYPOTHETICAL_EXTSTATS_MCV = 'm',
	HYPOTHETICAL_EXTSTATS_DEPENDENCIES = 'f'
} HypotheticalExtStatsKind;

/* Reset all repository and active-design state in the current backend. */
extern void hypothetical_extstats_reset(void);

/* Copy and validate one native serialized payload into backend-local state. */
extern void hypothetical_extstats_register(Oid statoid, Oid relid,
											HypotheticalExtStatsKind kind,
											const bytea *payload);

/* Replace the active design; ordered_oids defines effective precedence. */
extern void hypothetical_extstats_activate(List *ordered_oids);
extern void hypothetical_extstats_register_absent(Oid statoid, Oid relid,
										HypotheticalExtStatsKind kind);

/* Register a catalogless definition and return its backend-local identity. */
extern Oid hypothetical_extstats_register_definition(const char *candidate_id,
											Oid relid,
											HypotheticalExtStatsKind kind,
											Bitmapset *keys,
											const bytea *payload);
extern Oid hypothetical_extstats_register_definition_absent(const char *candidate_id,
											Oid relid,
											HypotheticalExtStatsKind kind,
											Bitmapset *keys);

/* Filter and reorder catalog definitions for the registered relation. */
extern List *hypothetical_extstats_filter(Oid relid, List *catalog_oids);

/* Return a registered native payload, or false when the overlay is inactive. */
extern bool hypothetical_extstats_payload(Oid statoid,
										  HypotheticalExtStatsKind kind,
										  const bytea **payload);
extern bool hypothetical_extstats_absent_native(Oid statoid,
										  HypotheticalExtStatsKind kind);
extern bool hypothetical_extstats_registered(Oid statoid,
										 HypotheticalExtStatsKind kind);
extern bool hypothetical_extstats_definition(Oid statoid, Oid relid,
											char *kind, Bitmapset **keys);

extern Datum pg_hypothetical_extstats_reset(PG_FUNCTION_ARGS);
extern Datum pg_hypothetical_extstats_register(PG_FUNCTION_ARGS);
extern Datum pg_hypothetical_extstats_register_absent(PG_FUNCTION_ARGS);
extern Datum pg_hypothetical_extstats_register_definition(PG_FUNCTION_ARGS);
extern Datum pg_hypothetical_extstats_register_definition_absent(PG_FUNCTION_ARGS);
extern Datum pg_hypothetical_extstats_activate(PG_FUNCTION_ARGS);
extern Datum pg_hypothetical_extstats_active(PG_FUNCTION_ARGS);

#endif							/* HYPOTHETICAL_H */
