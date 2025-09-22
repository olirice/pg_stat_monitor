/*-------------------------------------------------------------------------
 *
 * json_export.h
 *	  JSON export functionality for pg_stat_monitor bucket data
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
