-- Catalogless hypothetical extended-statistics definitions.
SET client_min_messages = warning;

CREATE TABLE catalogless_hypothetical_test (a integer, b integer, c integer);
INSERT INTO catalogless_hypothetical_test
SELECT i % 7, i % 7, i % 3
FROM generate_series(1, 1000) AS s(i);

-- Acquire native payloads once, then remove every physical definition.
CREATE STATISTICS catalogless_seed (mcv, dependencies)
ON a, b FROM catalogless_hypothetical_test;
ANALYZE catalogless_hypothetical_test;
CREATE TEMP TABLE catalogless_payload AS
SELECT d.stxdmcv AS mcv, d.stxddependencies AS fd
FROM pg_statistic_ext AS e
JOIN pg_statistic_ext_data AS d ON d.stxoid = e.oid
WHERE e.stxname = 'catalogless_seed';
DROP STATISTICS catalogless_seed;
SELECT count(*) = 0 AS no_physical_target
FROM pg_statistic_ext
WHERE stxrelid = 'catalogless_hypothetical_test'::regclass;

-- An unrelated physical object must remain outside the overlay.
CREATE TABLE catalogless_unrelated_test (x integer, y integer);
INSERT INTO catalogless_unrelated_test
SELECT i % 5, i % 11 FROM generate_series(1, 100) AS s(i);
CREATE STATISTICS catalogless_unrelated_stats (mcv)
ON x, y FROM catalogless_unrelated_test;
ANALYZE catalogless_unrelated_test;

SELECT pg_hypothetical_extstats_reset() AS reset_before;
SELECT pg_hypothetical_extstats_register_definition(
    'catalogless-mcv', 'catalogless_hypothetical_test'::regclass,
    'm', ARRAY[1, 2]::smallint[], (SELECT mcv FROM catalogless_payload)) AS mcv_oid
\gset
SELECT pg_hypothetical_extstats_register_definition(
    'catalogless-fd', 'catalogless_hypothetical_test'::regclass,
    'f', ARRAY[1, 2]::smallint[], (SELECT fd FROM catalogless_payload)) AS fd_oid
\gset
SELECT pg_hypothetical_extstats_register_definition_absent(
    'catalogless-absent', 'catalogless_hypothetical_test'::regclass,
    'm', ARRAY[1, 2]::smallint[]) AS absent_oid
\gset

-- Empty active state is the ordinary baseline and leaves unrelated objects.
SELECT pg_hypothetical_extstats_activate(ARRAY[]::oid[]) AS empty_active
\gset
SELECT pg_hypothetical_extstats_active() IS NOT NULL AS empty_set_visible;
EXPLAIN
SELECT * FROM catalogless_hypothetical_test WHERE a = 1 AND b = 1;
SELECT count(*) = 1 AS unrelated_preserved
FROM pg_statistic_ext
WHERE stxname = 'catalogless_unrelated_stats';

-- Each catalogless PRESENT kind is planner-visible without a catalog row.
SELECT pg_hypothetical_extstats_activate(ARRAY[:mcv_oid]::oid[]) AS activate_mcv
\gset
SELECT cardinality(pg_hypothetical_extstats_active()) = 1 AS mcv_active;
EXPLAIN
SELECT * FROM catalogless_hypothetical_test WHERE a = 1 AND b = 1;

SELECT pg_hypothetical_extstats_activate(ARRAY[:fd_oid]::oid[]) AS activate_fd
\gset
SELECT cardinality(pg_hypothetical_extstats_active()) = 1 AS fd_active;
EXPLAIN
SELECT * FROM catalogless_hypothetical_test WHERE a = 1 AND b = 1;

-- Multiple candidates can be switched deterministically.
SELECT pg_hypothetical_extstats_activate(ARRAY[:mcv_oid, :fd_oid]::oid[]) AS activate_both
\gset
SELECT cardinality(pg_hypothetical_extstats_active()) = 2 AS both_active;
EXPLAIN
SELECT * FROM catalogless_hypothetical_test WHERE a = 1 AND b = 1;

SELECT pg_hypothetical_extstats_activate(ARRAY[:absent_oid]::oid[]) AS activate_absent
\gset
SELECT cardinality(pg_hypothetical_extstats_active()) = 1 AS absent_active;
EXPLAIN
SELECT * FROM catalogless_hypothetical_test WHERE a = 1 AND b = 1;

-- Reset removes all backend-local identities and restores stock behavior.
SELECT pg_hypothetical_extstats_reset() AS reset_after;
SELECT pg_hypothetical_extstats_active() IS NULL AS reset_clears_active;
SELECT count(*) = 0 AS no_physical_target_after_reset
FROM pg_statistic_ext
WHERE stxrelid = 'catalogless_hypothetical_test'::regclass;
SELECT count(*) = 1 AS unrelated_preserved_after_reset
FROM pg_statistic_ext
WHERE stxname = 'catalogless_unrelated_stats';

DROP TABLE catalogless_unrelated_test;
DROP TABLE catalogless_hypothetical_test;
