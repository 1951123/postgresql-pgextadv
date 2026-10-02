-- Verify binary ANALYZE sample export/import and deterministic statistics.
SET client_min_messages = warning;
CREATE TABLE analyze_sample_cache_test (id integer, category text);
INSERT INTO analyze_sample_cache_test
SELECT i, 'category-' || (i % 17)
FROM generate_series(1, 2000) AS s(i);
ALTER TABLE analyze_sample_cache_test ALTER COLUMN category SET STATISTICS 1;

SET pgextadv.analyze_sample_export = '/tmp/pgextadv-analyze-sample-cache-regress.bin';
ANALYZE analyze_sample_cache_test;
SELECT (pg_stat_file('/tmp/pgextadv-analyze-sample-cache-regress.bin')).size > 0;

CREATE TEMP TABLE analyze_sample_cache_stats AS
SELECT attname, null_frac, avg_width, n_distinct,
       most_common_vals::text, most_common_freqs::text,
       histogram_bounds::text, correlation
FROM pg_stats
WHERE tablename = 'analyze_sample_cache_test';

SET pgextadv.analyze_sample_export = '';
SET pgextadv.analyze_sample_import = '/tmp/pgextadv-analyze-sample-cache-regress.bin';
ANALYZE analyze_sample_cache_test;
SELECT NOT EXISTS (
    (SELECT attname, null_frac, avg_width, n_distinct,
            most_common_vals::text, most_common_freqs::text,
            histogram_bounds::text, correlation
     FROM pg_stats
     WHERE tablename = 'analyze_sample_cache_test'
     EXCEPT
     SELECT attname, null_frac, avg_width, n_distinct,
            most_common_vals::text, most_common_freqs::text,
            histogram_bounds::text, correlation
     FROM analyze_sample_cache_stats)
    UNION ALL
    (SELECT attname, null_frac, avg_width, n_distinct,
            most_common_vals::text, most_common_freqs::text,
            histogram_bounds::text, correlation
     FROM analyze_sample_cache_stats
     EXCEPT
     SELECT attname, null_frac, avg_width, n_distinct,
            most_common_vals::text, most_common_freqs::text,
            histogram_bounds::text, correlation
     FROM pg_stats
     WHERE tablename = 'analyze_sample_cache_test')
);

SET pgextadv.analyze_sample_import = '';
DROP TABLE analyze_sample_cache_test;
