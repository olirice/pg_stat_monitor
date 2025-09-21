/*-------------------------------------------------------------------------
 *
 * json_export.h
 *	  JSON export functionality for pg_stat_monitor bucket data
 *
 * Portions Copyright © 2018-2024, Percona LLC and/or its affiliates
 *
 * IDENTIFICATION
 *	  contrib/pg_stat_monitor/json_export.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef JSON_EXPORT_H
#define JSON_EXPORT_H

#include "postgres.h"
#include "pg_stat_monitor.h"

/* Function to log bucket data as JSON */
extern void pgsm_log_bucket_json(uint64 bucket_id);

#endif /* JSON_EXPORT_H */