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

#include <math.h>
#include <sys/time.h>

#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"

#include "json_export.h"
#include "pg_stat_monitor.h"

/* Helper function to escape strings for JSON output */
static void escape_json(StringInfo buf, const char *str) {
	const char *p;

	appendStringInfoChar(buf, '"');
	for (p = str; *p; p++) {
		switch (*p) {
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
			if ((unsigned char)*p < 0x20)
				appendStringInfo(buf, "\\u%04x", (unsigned char)*p);
			else
				appendStringInfoChar(buf, *p);
			break;
		}
	}
	appendStringInfoChar(buf, '"');
}

/* Helper function to add a JSON string field with null handling */
static void add_json_string(StringInfo buf, const char *field_name,
                            const char *value, bool add_comma) {
	appendStringInfo(buf, "\"%s\":", field_name);
	if (value && value[0])
		escape_json(buf, value);
	else
		appendStringInfoString(buf, "null");

	if (add_comma)
		appendStringInfoString(buf, ",");
}

/* Helper function to convert client IP from integer to dotted decimal */
static void format_client_ip(StringInfo buf, uint32 ip) {
	if (ip == 0) {
		appendStringInfoString(buf, "null");
	} else {
		appendStringInfo(buf, "\"%u.%u.%u.%u\"", (ip >> 24) & 0xFF,
		                 (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
	}
}

/* Helper function to format relations array */
static void format_relations(StringInfo buf, QueryInfo *info) {
	int i;

	appendStringInfoString(buf, "[");
	for (i = 0; i < info->num_relations && i < REL_LST; i++) {
		if (i > 0)
			appendStringInfoString(buf, ",");
		if (info->relations[i][0])
			escape_json(buf, info->relations[i]);
		else
			appendStringInfoString(buf, "null");
	}
	appendStringInfoString(buf, "]");
}

/* Helper function to format histogram/response calls array */
static void format_resp_calls(StringInfo buf, int *resp_calls) {
	int i;

	appendStringInfoString(buf, "[");
	for (i = 0; i < MAX_RESPONSE_BUCKET; i++) {
		if (i > 0)
			appendStringInfoString(buf, ",");
		appendStringInfo(buf, "%d", resp_calls[i]);
	}
	appendStringInfoString(buf, "]");
}

/* Helper to calculate standard deviation from sum of variances */
static double calculate_stddev_time(double sum_var_time, int64 calls) {
	if (calls <= 1)
		return 0.0;
	return sqrt(sum_var_time / (calls - 1));
}

/*
 * Log bucket data as JSON to PostgreSQL log - one log entry per query for
 * scalability Includes all fields from the SQL view with proper type handling
 * and null safety
 */
void pgsm_log_bucket_json(uint64 bucket_id) {
	pgsmSharedState *pgsm;
	PGSM_HASH_SEQ_STATUS hstat;
	pgsmEntry *entry;
	struct timeval start_time, end_time;
	double duration_seconds;
	time_t rotation_timestamp;
	int query_count = 0;
	int queries_exported = 0;
	dsa_area *query_dsa_area = NULL;

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
	while ((entry = pgsm_hash_seq_next(&hstat)) != NULL) {
		if (entry->key.bucket_id == bucket_id)
			query_count++;
	}
	pgsm_hash_seq_term(&hstat);

	/* Log bucket rotation start with metadata */
	elog(
	    LOG,
	    "[pg_stat_monitor] JSON export: {\"event\": \"bucket_rotation_start\", "
	    "\"bucket_id\": %lu, \"timestamp\": %ld, \"query_count\": %d}",
	    bucket_id, rotation_timestamp, query_count);

	if (query_count > 0) {
		/* Second pass: export each query as comprehensive JSON */
		pgsm_hash_seq_init(&hstat, get_pgsmHash(), true);
		while ((entry = pgsm_hash_seq_next(&hstat)) != NULL) {
			if (entry->key.bucket_id == bucket_id) {
				StringInfoData json;
				char *query_text = NULL;
				char *top_query_text = NULL;
				char *plan_text = NULL;
				TimestampTz bucket_start_time = 0;
				double stddev_exec_time;
				double stddev_plan_time;
				bool bucket_done;

				queries_exported++;

				/* Get query text from DSA or local pointer */
				if (DsaPointerIsValid(entry->query_text.query_pos) &&
				    query_dsa_area) {
					char *query_ptr = dsa_get_address(
					    query_dsa_area, entry->query_text.query_pos);
					query_text = pstrdup(query_ptr);
				} else if (entry->query_text.query_pointer) {
					query_text = pstrdup(entry->query_text.query_pointer);
				}

				/* Get top query text - currently same as query text in most
				 * cases */
				top_query_text = query_text;

				/* Get plan text if available */
				if (entry->counters.planinfo.plan_len > 0)
					plan_text = entry->counters.planinfo.plan_text;

				/* Get bucket start time if available */
				if (bucket_id < pgsm_max_buckets)
					bucket_start_time = pgsm->bucket_start_time[bucket_id];

				/* Calculate derived values */
				stddev_exec_time =
				    calculate_stddev_time(entry->counters.time.sum_var_time,
				                          entry->counters.calls.calls);
				stddev_plan_time =
				    calculate_stddev_time(entry->counters.plantime.sum_var_time,
				                          entry->counters.plancalls.calls);
				bucket_done =
				    (bucket_id != pg_atomic_read_u64(&pgsm->current_wbucket));

				/* Build comprehensive JSON using single template - FLATTENED
				 * structure */
				initStringInfo(&json);
				appendStringInfo(&json,
				                 "[pg_stat_monitor] JSON export: {"
				                 "\"event\":\"bucket_query\","
				                 "\"bucket_id\":%lu,"
				                 "\"timestamp\":%ld,"
				                 "\"query_index\":%d,"
				                 "\"query_count\":%d,"

				                 /* Basic identification */
				                 "\"bucket\":%lu,"
				                 "\"bucket_start_time\":%ld,"
				                 "\"userid\":%u,",
				                 bucket_id, rotation_timestamp,
				                 queries_exported, query_count,
				                 entry->key.bucket_id, (long)bucket_start_time,
				                 entry->key.userid);

				/* Handle username with proper escaping */
				add_json_string(&json, "username",
				                entry->username[0] ? entry->username : NULL,
				                true);

				appendStringInfo(&json, "\"dbid\":%u,", entry->key.dbid);

				/* Handle datname with proper escaping */
				add_json_string(&json, "datname",
				                entry->datname[0] ? entry->datname : NULL,
				                true);

				appendStringInfoString(&json, "\"client_ip\":");
				format_client_ip(&json, entry->key.ip);

				/* Query identification fields */
				appendStringInfo(
				    &json,
				    ",\"pgsm_query_id\":%lu,"
				    "\"queryid\":%lu,"
				    "\"planid\":%lu,"
				    "\"top_queryid\":%lu,"
				    "\"toplevel\":%s,",
				    entry->pgsm_query_id, entry->key.queryid, entry->key.planid,
				    entry->key
				        .queryid, /* top_queryid is typically same as queryid */
				    "true" /* toplevel - we don't track nested calls in current
				              implementation */
				);

				/* Query texts */
				appendStringInfoString(&json, "\"query\":");
				if (query_text)
					escape_json(&json, query_text);
				else
					appendStringInfoString(&json, "null");

				add_json_string(&json, "query_plan", plan_text, true);
				add_json_string(&json, "top_query", top_query_text, true);
				add_json_string(&json, "application_name",
				                entry->counters.info.application_name[0]
				                    ? entry->counters.info.application_name
				                    : NULL,
				                true);
				add_json_string(&json, "comments",
				                entry->counters.info.comments[0]
				                    ? entry->counters.info.comments
				                    : NULL,
				                true);

				/* Relations array */
				appendStringInfoString(&json, "\"relations\":");
				format_relations(&json, &entry->counters.info);

				/* Command type and error info */
				appendStringInfo(&json,
				                 ",\"cmd_type\":%d,"
				                 "\"elevel\":%ld,",
				                 (int)entry->counters.info.cmd_type,
				                 entry->counters.error.elevel);

				add_json_string(&json, "sqlcode",
				                entry->counters.error.sqlcode[0]
				                    ? entry->counters.error.sqlcode
				                    : NULL,
				                true);
				add_json_string(&json, "message",
				                entry->counters.error.message[0]
				                    ? entry->counters.error.message
				                    : NULL,
				                true);

				/* Execution statistics */
				appendStringInfo(&json,
				                 "\"calls\":%ld,"
				                 "\"total_exec_time\":%.3f,"
				                 "\"min_exec_time\":%.3f,"
				                 "\"max_exec_time\":%.3f,"
				                 "\"mean_exec_time\":%.3f,"
				                 "\"stddev_exec_time\":%.3f,"
				                 "\"rows\":%ld,",
				                 entry->counters.calls.calls,
				                 entry->counters.time.total_time,
				                 entry->counters.time.min_time,
				                 entry->counters.time.max_time,
				                 entry->counters.time.mean_time,
				                 stddev_exec_time, entry->counters.calls.rows);

				/* Planning statistics */
				appendStringInfo(&json,
				                 "\"plans\":%ld,"
				                 "\"total_plan_time\":%.3f,"
				                 "\"min_plan_time\":%.3f,"
				                 "\"max_plan_time\":%.3f,"
				                 "\"mean_plan_time\":%.3f,"
				                 "\"stddev_plan_time\":%.3f,",
				                 entry->counters.plancalls.calls,
				                 entry->counters.plantime.total_time,
				                 entry->counters.plantime.min_time,
				                 entry->counters.plantime.max_time,
				                 entry->counters.plantime.mean_time,
				                 stddev_plan_time);

				/* Block I/O statistics */
				appendStringInfo(&json,
				                 "\"shared_blks_hit\":%ld,"
				                 "\"shared_blks_read\":%ld,"
				                 "\"shared_blks_dirtied\":%ld,"
				                 "\"shared_blks_written\":%ld,"
				                 "\"local_blks_hit\":%ld,"
				                 "\"local_blks_read\":%ld,"
				                 "\"local_blks_dirtied\":%ld,"
				                 "\"local_blks_written\":%ld,"
				                 "\"temp_blks_read\":%ld,"
				                 "\"temp_blks_written\":%ld,"
				                 "\"shared_blk_read_time\":%.3f,"
				                 "\"shared_blk_write_time\":%.3f,"
				                 "\"local_blk_read_time\":%.3f,"
				                 "\"local_blk_write_time\":%.3f,"
				                 "\"temp_blk_read_time\":%.3f,"
				                 "\"temp_blk_write_time\":%.3f,",
				                 entry->counters.blocks.shared_blks_hit,
				                 entry->counters.blocks.shared_blks_read,
				                 entry->counters.blocks.shared_blks_dirtied,
				                 entry->counters.blocks.shared_blks_written,
				                 entry->counters.blocks.local_blks_hit,
				                 entry->counters.blocks.local_blks_read,
				                 entry->counters.blocks.local_blks_dirtied,
				                 entry->counters.blocks.local_blks_written,
				                 entry->counters.blocks.temp_blks_read,
				                 entry->counters.blocks.temp_blks_written,
				                 entry->counters.blocks.shared_blk_read_time,
				                 entry->counters.blocks.shared_blk_write_time,
				                 entry->counters.blocks.local_blk_read_time,
				                 entry->counters.blocks.local_blk_write_time,
				                 entry->counters.blocks.temp_blk_read_time,
				                 entry->counters.blocks.temp_blk_write_time);

				/* Response calls histogram */
				appendStringInfoString(&json, "\"resp_calls\":");
				format_resp_calls(&json, entry->counters.resp_calls);

				/* System info */
				appendStringInfo(&json,
				                 ",\"cpu_user_time\":%.3f,"
				                 "\"cpu_sys_time\":%.3f,",
				                 entry->counters.sysinfo.utime,
				                 entry->counters.sysinfo.stime);

				/* WAL usage */
				appendStringInfo(&json,
				                 "\"wal_records\":%ld,"
				                 "\"wal_fpi\":%ld,"
				                 "\"wal_bytes\":%lu,",
				                 entry->counters.walusage.wal_records,
				                 entry->counters.walusage.wal_fpi,
				                 entry->counters.walusage.wal_bytes);

				/* JIT statistics */
				appendStringInfo(&json,
				                 "\"jit_functions\":%ld,"
				                 "\"jit_generation_time\":%.3f,"
				                 "\"jit_inlining_count\":%ld,"
				                 "\"jit_inlining_time\":%.3f,"
				                 "\"jit_optimization_count\":%ld,"
				                 "\"jit_optimization_time\":%.3f,"
				                 "\"jit_emission_count\":%ld,"
				                 "\"jit_emission_time\":%.3f,"
				                 "\"jit_deform_count\":%ld,"
				                 "\"jit_deform_time\":%.3f,",
				                 entry->counters.jitinfo.jit_functions,
				                 entry->counters.jitinfo.jit_generation_time,
				                 entry->counters.jitinfo.jit_inlining_count,
				                 entry->counters.jitinfo.jit_inlining_time,
				                 entry->counters.jitinfo.jit_optimization_count,
				                 entry->counters.jitinfo.jit_optimization_time,
				                 entry->counters.jitinfo.jit_emission_count,
				                 entry->counters.jitinfo.jit_emission_time,
				                 entry->counters.jitinfo.jit_deform_count,
				                 entry->counters.jitinfo.jit_deform_time);

				/* Timestamps and bucket status */
				appendStringInfo(&json,
				                 "\"stats_since\":%ld,"
				                 "\"minmax_stats_since\":%ld,"
				                 "\"bucket_done\":%s}",
				                 (long)entry->stats_since,
				                 (long)entry->minmax_stats_since,
				                 bucket_done ? "true" : "false");

				/* Log this comprehensive query record */
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

	elog(LOG,
	     "[pg_stat_monitor] JSON export: {\"event\": \"bucket_rotation_end\", "
	     "\"bucket_id\": %lu, \"timestamp\": %ld, \"duration_seconds\": %.3f, "
	     "\"queries_exported\": %d}",
	     bucket_id, rotation_timestamp, duration_seconds, queries_exported);
}