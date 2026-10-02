# pg-extstats-advisor PostgreSQL source

This repository is the authoritative experimental PostgreSQL source used by
`pg-extstats-advisor`. It retains official PostgreSQL Git ancestry.

- Upstream base tag: `REL_16_14`
- Upstream base commit: `0d1c00c624fa7367d4a895f44381887757289682`
- Authoritative branch: `main`

The source currently contains two pgextadv capabilities:

- a backend-local hypothetical extended-statistics substrate;
- deterministic `ANALYZE` sample export/import using the
  `pgextadv.analyze_sample_export` and `pgextadv.analyze_sample_import` GUCs.

The repository is the source of truth for those changes. `pg-extstats-advisor`
consumes a frozen commit from this history; any PostgreSQL patch stored there
is a derived reproduction artifact generated from the upstream base and that
commit. This repository makes no claim of upstream PostgreSQL support or
acceptance.
