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
#include "utils/inet.h"
#include "lib/stringinfo.h"
#include "executor/spi.h"
#include "utils/memutils.h"
#include "json_export.h"
#include "pg_stat_monitor.h"


/*
 * Log bucket data as JSON to PostgreSQL log - one log entry per query for scalability
 */
void
pgsm_log_bucket_json(uint64 bucket_id)
{
	StringInfoData json;
	int			ret;
	MemoryContext old_context;
	struct timeval start_time,
				end_time;
	double		duration_seconds;
	time_t		rotation_timestamp;
	int			query_count = 0;

	if (!pgsm_enable_json_log)
		return;

	/* Record start time and timestamp for duration calculation */
	gettimeofday(&start_time, NULL);
	rotation_timestamp = time(NULL);

	/* Initialize JSON output buffer */
	initStringInfo(&json);

	/* Try to execute a SQL query to get the bucket data */
	old_context = CurrentMemoryContext;

	PG_TRY();
	{
		/* Start a transaction for SQL execution */
		if (SPI_connect() != SPI_OK_CONNECT)
		{
			/* Failed to connect - likely in background context */
			gettimeofday(&end_time, NULL);
			duration_seconds = (end_time.tv_sec - start_time.tv_sec) +
				(end_time.tv_usec - start_time.tv_usec) / 1000000.0;
			elog(LOG, "[pg_stat_monitor] JSON export: {\"event\": \"bucket_rotation_error\", \"bucket_id\": %lu, \"timestamp\": %ld, \"duration_seconds\": %.3f, \"note\": \"SPI connection failed - background context\"}",
				 bucket_id, rotation_timestamp, duration_seconds);
			pfree(json.data);
			return;
		}

		/* First get count of queries in this bucket */
		appendStringInfo(&json, "SELECT COUNT(*) FROM pg_stat_monitor WHERE bucket = %lu", bucket_id);
		ret = SPI_execute(json.data, true, 0);

		if (ret == SPI_OK_SELECT && SPI_processed > 0)
		{
			bool		isnull;
			Datum		count_datum = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);

			if (!isnull)
				query_count = DatumGetInt32(count_datum);
		}

		/* Log bucket rotation start with metadata */
		elog(LOG, "[pg_stat_monitor] JSON export: {\"event\": \"bucket_rotation_start\", \"bucket_id\": %lu, \"timestamp\": %ld, \"query_count\": %d}",
			 bucket_id, rotation_timestamp, query_count);

		if (query_count > 0)
		{
			/* Reset and build query to get individual query records */
			resetStringInfo(&json);
			appendStringInfo(&json,
							 "SELECT row_to_json(t) FROM ("
							 "SELECT bucket, bucket_start_time, userid, username, dbid, datname, "
							 "client_ip, pgsm_query_id, queryid, toplevel, top_queryid, query, "
							 "comments, planid, query_plan, top_query, application_name, relations, "
							 "cmd_type, cmd_type_text, elevel, sqlcode, message, calls, "
							 "total_exec_time, min_exec_time, max_exec_time, mean_exec_time, "
							 "stddev_exec_time, rows, shared_blks_hit, shared_blks_read, "
							 "shared_blks_dirtied, shared_blks_written, local_blks_hit, "
							 "local_blks_read, local_blks_dirtied, local_blks_written, "
							 "temp_blks_read, temp_blks_written, shared_blk_read_time, "
							 "shared_blk_write_time, local_blk_read_time, local_blk_write_time, "
							 "temp_blk_read_time, temp_blk_write_time, resp_calls, cpu_user_time, "
							 "cpu_sys_time, wal_records, wal_fpi, wal_bytes, bucket_done, plans, "
							 "total_plan_time, min_plan_time, max_plan_time, mean_plan_time, "
							 "stddev_plan_time, jit_functions, jit_generation_time, "
							 "jit_inlining_count, jit_inlining_time, jit_optimization_count, "
							 "jit_optimization_time, jit_emission_count, jit_emission_time "
							 "FROM pg_stat_monitor WHERE bucket = %lu"
							 ") t",
							 bucket_id);

			ret = SPI_execute(json.data, true, 0);

			if (ret == SPI_OK_SELECT && SPI_processed > 0)
			{
				/* Log each query as a separate JSON entry */
				for (uint64 i = 0; i < SPI_processed; i++)
				{
					bool		isnull;
					Datum		result_datum = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull);

					if (!isnull)
					{
						StringInfoData log_msg;
						char	   *query_json = TextDatumGetCString(result_datum);

						/*
						 * Build log message safely without format string
						 * issues
						 */
						initStringInfo(&log_msg);
						appendStringInfo(&log_msg,
										 "[pg_stat_monitor] JSON export: {\"event\": \"bucket_query\", \"bucket_id\": %lu, \"timestamp\": %ld, \"query_index\": %lu, \"query_count\": %d, \"query\": ",
										 bucket_id, rotation_timestamp, i + 1, query_count);
						appendStringInfoString(&log_msg, query_json);
						appendStringInfoChar(&log_msg, '}');

						ereport(LOG, (errmsg_internal("%s", log_msg.data)));

						pfree(log_msg.data);
						if (query_json)
							pfree(query_json);
					}
				}
			}
		}

		/* Calculate final duration and log bucket rotation end */
		gettimeofday(&end_time, NULL);
		duration_seconds = (end_time.tv_sec - start_time.tv_sec) +
			(end_time.tv_usec - start_time.tv_usec) / 1000000.0;

		elog(LOG, "[pg_stat_monitor] JSON export: {\"event\": \"bucket_rotation_end\", \"bucket_id\": %lu, \"timestamp\": %ld, \"duration_seconds\": %.3f, \"queries_exported\": %d}",
			 bucket_id, rotation_timestamp, duration_seconds, query_count);

		SPI_finish();
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(old_context);

		/*
		 * SPI calls can fail in background processes, fall back to simple
		 * logging
		 */
		gettimeofday(&end_time, NULL);
		duration_seconds = (end_time.tv_sec - start_time.tv_sec) +
			(end_time.tv_usec - start_time.tv_usec) / 1000000.0;
		elog(LOG, "[pg_stat_monitor] JSON export: {\"event\": \"bucket_rotation_error\", \"bucket_id\": %lu, \"timestamp\": %ld, \"duration_seconds\": %.3f, \"note\": \"Export failed - not available in background context\"}",
			 bucket_id, rotation_timestamp, duration_seconds);

		FlushErrorState();
	}
	PG_END_TRY();

	pfree(json.data);
}
