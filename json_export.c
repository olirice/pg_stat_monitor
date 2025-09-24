/*-------------------------------------------------------------------------
 *
 * json_export.c
 *	  JSON export functionality for pg_stat_monitor bucket data
 *
 * Portions Copyright © 2018-2024, Percona LLC and/or its affiliates
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 1994, The Regents of the University of California
 *
 * IDENTIFICATION
 *	  contrib/pg_stat_monitor/json_export.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include <sys/time.h>
#include "utils/builtins.h"
#include "lib/stringinfo.h"
#include "utils/timestamp.h"
#include "json_export.h"
#include "pg_stat_monitor.h"

/* Helper function to escape strings for JSON output */
static void
escape_json(StringInfo buf, const char *str)
{
	const char *p;

	appendStringInfoChar(buf, '"');
	for (p = str; *p; p++)
	{
		switch (*p)
		{
			case '"':
				appendStringInfoString(buf, "\\\"");
				break;
			case '\\':
				appendStringInfoString(buf, "\\\\");
				break;
			case '\b':
				appendStringInfoString(buf, "\\b");
				break;
			case '\f':
				appendStringInfoString(buf, "\\f");
				break;
			case '\n':
				appendStringInfoString(buf, "\\n");
				break;
			case '\r':
				appendStringInfoString(buf, "\\r");
				break;
			case '\t':
				appendStringInfoString(buf, "\\t");
				break;
			default:
				if ((unsigned char) *p < 0x20)
					appendStringInfo(buf, "\\u%04x", (unsigned char) *p);
				else
					appendStringInfoChar(buf, *p);
				break;
		}
	}
	appendStringInfoChar(buf, '"');
}

/*
 * Log bucket data as JSON to PostgreSQL log - one log entry per query for scalability
 */
void
pgsm_log_bucket_json(uint64 bucket_id)
{
	pgsmSharedState *pgsm;
	PGSM_HASH_SEQ_STATUS hstat;
	pgsmEntry  *entry;
	struct timeval start_time,
				end_time;
	double		duration_seconds;
	time_t		rotation_timestamp;
	int			query_count = 0;
	int			queries_exported = 0;
	dsa_area   *query_dsa_area = NULL;

	if (!pgsm_enable_json_log)
		return;

	/* Record start time and timestamp for duration calculation */
	gettimeofday(&start_time, NULL);
	rotation_timestamp = time(NULL);

	/* Get shared state and DSA area for query text */
	pgsm = pgsm_get_ss();
	query_dsa_area = get_dsa_area_for_query_text();

	/* First pass: count queries in this bucket */
	pgsm_hash_seq_init(&hstat, get_pgsmHash(), true);
	while ((entry = pgsm_hash_seq_next(&hstat)) != NULL)
	{
		if (entry->key.bucket_id == bucket_id)
			query_count++;
	}
	pgsm_hash_seq_term(&hstat);

	/* Log bucket rotation start with metadata */
	elog(LOG, "[pg_stat_monitor] JSON export: {\"event\": \"bucket_rotation_start\", \"bucket_id\": %lu, \"timestamp\": %ld, \"query_count\": %d}",
		 bucket_id, rotation_timestamp, query_count);

	if (query_count > 0)
	{
		/* Second pass: export each query as JSON */
		pgsm_hash_seq_init(&hstat, get_pgsmHash(), true);
		while ((entry = pgsm_hash_seq_next(&hstat)) != NULL)
		{
			if (entry->key.bucket_id == bucket_id)
			{
				StringInfoData json;
				char	   *query_text = NULL;
				char	   *username;
				char	   *datname;
				TimestampTz bucket_start_time = 0;

				queries_exported++;

				/* Get query text from DSA or local pointer */
				if (DsaPointerIsValid(entry->query_text.query_pos) && query_dsa_area)
				{
					char	   *query_ptr = dsa_get_address(query_dsa_area, entry->query_text.query_pos);

					query_text = pstrdup(query_ptr);
				}
				else if (entry->query_text.query_pointer)
				{
					query_text = pstrdup(entry->query_text.query_pointer);
				}
				else
				{
					query_text = pstrdup("Query text not available");
				}

				/* Get bucket start time if available */
				if (bucket_id < pgsm_max_buckets)
					bucket_start_time = pgsm->bucket_start_time[bucket_id];

				/* Get username and database name */
				username = entry->username[0] ? entry->username : "unknown";
				datname = entry->datname[0] ? entry->datname : "unknown";

				/* Build JSON for this query */
				initStringInfo(&json);
				appendStringInfo(&json,
								 "[pg_stat_monitor] JSON export: {\"event\": \"bucket_query\", "
								 "\"bucket_id\": %lu, "
								 "\"timestamp\": %ld, "
								 "\"query_index\": %d, "
								 "\"query_count\": %d, "
								 "\"query\": {",
								 bucket_id, rotation_timestamp, queries_exported, query_count);

				/* Basic identification fields */
				appendStringInfo(&json,
								 "\"bucket\": %lu, "
								 "\"bucket_start_time\": %ld, "
								 "\"userid\": %u, "
								 "\"username\": \"%s\", "
								 "\"dbid\": %u, "
								 "\"datname\": \"%s\", "
								 "\"queryid\": %lu, "
								 "\"pgsm_query_id\": %lu, ",
								 entry->key.bucket_id,
								 (long) bucket_start_time,
								 entry->key.userid,
								 username,
								 entry->key.dbid,
								 datname,
								 entry->key.queryid,
								 entry->pgsm_query_id);

				/* Query text - needs special escaping */
				appendStringInfo(&json, "\"query\": ");
				escape_json(&json, query_text);
				appendStringInfo(&json, ", ");

				/* Command type */
				appendStringInfo(&json,
								 "\"cmd_type\": %d, ",
								 entry->counters.info.cmd_type);

				/* Execution statistics */
				appendStringInfo(&json,
								 "\"calls\": %ld, "
								 "\"total_exec_time\": %.3f, "
								 "\"min_exec_time\": %.3f, "
								 "\"max_exec_time\": %.3f, "
								 "\"mean_exec_time\": %.3f, "
								 "\"rows\": %ld, ",
								 entry->counters.calls.calls,
								 entry->counters.time.total_time,
								 entry->counters.time.min_time,
								 entry->counters.time.max_time,
								 entry->counters.time.mean_time,
								 entry->counters.calls.rows);

				/* Block I/O statistics */
				appendStringInfo(&json,
								 "\"shared_blks_hit\": %ld, "
								 "\"shared_blks_read\": %ld, "
								 "\"shared_blks_dirtied\": %ld, "
								 "\"shared_blks_written\": %ld, "
								 "\"local_blks_hit\": %ld, "
								 "\"local_blks_read\": %ld, "
								 "\"local_blks_dirtied\": %ld, "
								 "\"local_blks_written\": %ld, "
								 "\"temp_blks_read\": %ld, "
								 "\"temp_blks_written\": %ld, ",
								 entry->counters.blocks.shared_blks_hit,
								 entry->counters.blocks.shared_blks_read,
								 entry->counters.blocks.shared_blks_dirtied,
								 entry->counters.blocks.shared_blks_written,
								 entry->counters.blocks.local_blks_hit,
								 entry->counters.blocks.local_blks_read,
								 entry->counters.blocks.local_blks_dirtied,
								 entry->counters.blocks.local_blks_written,
								 entry->counters.blocks.temp_blks_read,
								 entry->counters.blocks.temp_blks_written);

				/* WAL usage */
				appendStringInfo(&json,
								 "\"wal_records\": %ld, "
								 "\"wal_fpi\": %ld, "
								 "\"wal_bytes\": %lu",
								 entry->counters.walusage.wal_records,
								 entry->counters.walusage.wal_fpi,
								 entry->counters.walusage.wal_bytes);

				appendStringInfoString(&json, "}}");

				/* Log this query */
				ereport(LOG, (errmsg_internal("%s", json.data)));

				pfree(json.data);
				if (query_text)
					pfree(query_text);
			}
		}
		pgsm_hash_seq_term(&hstat);
	}

	/* Calculate final duration and log bucket rotation end */
	gettimeofday(&end_time, NULL);
	duration_seconds = (end_time.tv_sec - start_time.tv_sec) +
		(end_time.tv_usec - start_time.tv_usec) / 1000000.0;

	elog(LOG, "[pg_stat_monitor] JSON export: {\"event\": \"bucket_rotation_end\", \"bucket_id\": %lu, \"timestamp\": %ld, \"duration_seconds\": %.3f, \"queries_exported\": %d}",
		 bucket_id, rotation_timestamp, duration_seconds, queries_exported);
}