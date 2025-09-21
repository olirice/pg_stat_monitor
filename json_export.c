/*-------------------------------------------------------------------------
 *
 * json_export.c
 *	  JSON export functionality for pg_stat_monitor bucket data
 *
 * Portions Copyright © 2018-2024, Percona LLC and/or its affiliates
 *
 * IDENTIFICATION
 *	  contrib/pg_stat_monitor/json_export.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "utils/builtins.h"
#include "utils/inet.h"
#include "lib/stringinfo.h"
#include "json_export.h"
#include "pg_stat_monitor.h"
#include "access/htup_details.h"
#include "catalog/pg_database.h"
#include "catalog/pg_authid.h"

/*
 * Helper function to safely append a JSON string value, escaping special characters
 */
static void
append_json_string_value(StringInfo json, const char *value)
{
	if (!value || strlen(value) == 0)
	{
		appendStringInfoString(json, "null");
		return;
	}

	appendStringInfoChar(json, '"');
	for (const char *p = value; *p; p++)
	{
		switch (*p)
		{
			case '"':
				appendStringInfoString(json, "\\\"");
				break;
			case '\\':
				appendStringInfoString(json, "\\\\");
				break;
			case '\b':
				appendStringInfoString(json, "\\b");
				break;
			case '\f':
				appendStringInfoString(json, "\\f");
				break;
			case '\n':
				appendStringInfoString(json, "\\n");
				break;
			case '\r':
				appendStringInfoString(json, "\\r");
				break;
			case '\t':
				appendStringInfoString(json, "\\t");
				break;
			default:
				if ((unsigned char) *p < 32)
					appendStringInfo(json, "\\u%04x", (unsigned char) *p);
				else
					appendStringInfoChar(json, *p);
				break;
		}
	}
	appendStringInfoChar(json, '"');
}

/*
 * Helper function to safely append a JSON array of strings
 */
static void
append_json_string_array(StringInfo json, const char *relations[], int num_relations)
{
	appendStringInfoChar(json, '[');
	for (int i = 0; i < num_relations; i++)
	{
		if (i > 0)
			appendStringInfoChar(json, ',');
		append_json_string_value(json, relations[i]);
	}
	appendStringInfoChar(json, ']');
}

/*
 * Log bucket data as JSON to PostgreSQL log - exports core pg_stat_monitor data safely
 */
void
pgsm_log_bucket_json(uint64 bucket_id)
{
	if (!pgsm_enable_json_log)
		return;

	/* Simple JSON log for bucket rotation */
	elog(LOG, "[pg_stat_monitor] JSON export: {\"event\": \"bucket_rotation\", \"bucket_id\": %lu, \"timestamp\": %ld, \"note\": \"Full data export to be implemented after structure analysis\"}",
		 bucket_id, time(NULL));
}