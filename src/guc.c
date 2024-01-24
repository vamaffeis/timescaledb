/*
 * This file and its contents are licensed under the Apache License 2.0.
 * Please see the included NOTICE for copyright information and
 * LICENSE-APACHE for a copy of the license.
 */
#include <postgres.h>
#include <utils/guc.h>
#include <miscadmin.h>

#include "guc.h"
#include "license_guc.h"
#include "config.h"
#include "hypertable_cache.h"
#ifdef USE_TELEMETRY
#include "telemetry/telemetry.h"
#endif

#ifdef USE_TELEMETRY
/* Define which level means on. We use this object to have at least one object
 * of type TelemetryLevel in the code, otherwise pgindent won't work for the
 * type */
static const TelemetryLevel on_level = TELEMETRY_NO_FUNCTIONS;

bool
ts_telemetry_on()
{
	return ts_guc_telemetry_level >= on_level;
}

bool
ts_function_telemetry_on()
{
	return ts_guc_telemetry_level > TELEMETRY_NO_FUNCTIONS;
}

static const struct config_enum_entry telemetry_level_options[] = {
	{ "off", TELEMETRY_OFF, false },
	{ "no_functions", TELEMETRY_NO_FUNCTIONS, false },
	{ "basic", TELEMETRY_BASIC, false },
	{ NULL, 0, false }
};
#endif

/* Copied from contrib/auto_explain/auto_explain.c */
static const struct config_enum_entry loglevel_options[] = {
	{ "debug5", DEBUG5, false }, { "debug4", DEBUG4, false }, { "debug3", DEBUG3, false },
	{ "debug2", DEBUG2, false }, { "debug1", DEBUG1, false }, { "debug", DEBUG2, true },
	{ "info", INFO, false },	 { "notice", NOTICE, false }, { "warning", WARNING, false },
	{ "log", LOG, false },		 { NULL, 0, false }
};

bool ts_guc_enable_deprecation_warnings = true;
bool ts_guc_enable_optimizations = true;
bool ts_guc_restoring = false;
bool ts_guc_enable_constraint_aware_append = true;
bool ts_guc_enable_ordered_append = true;
bool ts_guc_enable_chunk_append = true;
bool ts_guc_enable_parallel_chunk_append = true;
bool ts_guc_enable_runtime_exclusion = true;
bool ts_guc_enable_constraint_exclusion = true;
bool ts_guc_enable_qual_propagation = true;
bool ts_guc_enable_cagg_reorder_groupby = true;
bool ts_guc_enable_now_constify = true;
TSDLLEXPORT bool ts_guc_enable_cagg_watermark_constify = true;
bool ts_guc_enable_osm_reads = true;
TSDLLEXPORT bool ts_guc_enable_dml_decompression = true;
TSDLLEXPORT bool ts_guc_enable_transparent_decompression = true;
TSDLLEXPORT bool ts_guc_enable_decompression_logrep_markers = false;
TSDLLEXPORT bool ts_guc_enable_decompression_sorted_merge = true;
bool ts_guc_enable_async_append = true;
bool ts_guc_enable_chunkwise_aggregation = true;
bool ts_guc_enable_vectorized_aggregation = true;
TSDLLEXPORT bool ts_guc_enable_compression_indexscan = true;
TSDLLEXPORT bool ts_guc_enable_bulk_decompression = true;
TSDLLEXPORT int ts_guc_bgw_log_level = WARNING;
TSDLLEXPORT bool ts_guc_enable_skip_scan = true;
/* default value of ts_guc_max_open_chunks_per_insert and ts_guc_max_cached_chunks_per_hypertable
 * will be set as their respective boot-value when the GUC mechanism starts up */
int ts_guc_max_open_chunks_per_insert;
int ts_guc_max_cached_chunks_per_hypertable;
#ifdef USE_TELEMETRY
TelemetryLevel ts_guc_telemetry_level = TELEMETRY_DEFAULT;
char *ts_telemetry_cloud = NULL;
#endif

TSDLLEXPORT char *ts_guc_license = TS_LICENSE_DEFAULT;
char *ts_last_tune_time = NULL;
char *ts_last_tune_version = NULL;
TSDLLEXPORT int ts_guc_max_insert_batch_size = 1000;

bool ts_guc_debug_require_batch_sorted_merge = false;

#ifdef TS_DEBUG
bool ts_shutdown_bgw = false;
char *ts_current_timestamp_mock = NULL;
#endif

#ifdef TS_DEBUG
static const struct config_enum_entry require_vector_qual_options[] = {
	{ "allow", RVQ_Allow, false },
	{ "forbid", RVQ_Forbid, false },
	{ "only", RVQ_Only, false },
	{ NULL, 0, false }
};
#endif

DebugRequireVectorQual ts_guc_debug_require_vector_qual = RVQ_Allow;
bool ts_guc_debug_compression_path_info = false;

static bool ts_guc_enable_hypertable_create = true;
static bool ts_guc_enable_hypertable_compression = true;
static bool ts_guc_enable_cagg_create = true;
static bool ts_guc_enable_policy_create = true;

typedef struct
{
	const char *name;
	const char *description;
	bool *enable;
} FeatureFlag;

static FeatureFlag ts_feature_flags[] = {
	[FEATURE_HYPERTABLE] = { "timescaledb.enable_hypertable_create",
							 "Enable creation of hypertable",
							 &ts_guc_enable_hypertable_create },

	[FEATURE_HYPERTABLE_COMPRESSION] = { "timescaledb.enable_hypertable_compression",
										 "Enable hypertable compression functions",
										 &ts_guc_enable_hypertable_compression },

	[FEATURE_CAGG] = { "timescaledb.enable_cagg_create",
					   "Enable creation of continuous aggregate",
					   &ts_guc_enable_cagg_create },

	[FEATURE_POLICY] = { "timescaledb.enable_policy_create",
						 "Enable creation of policies and user-defined actions",
						 &ts_guc_enable_policy_create }
};

static void
ts_feature_flag_add(FeatureFlagType type)
{
	FeatureFlag *flag = &ts_feature_flags[type];
	int flag_context = PGC_SIGHUP;
#ifdef TS_DEBUG
	flag_context = PGC_USERSET;
#endif
	DefineCustomBoolVariable(flag->name,
							 flag->description,
							 NULL,
							 flag->enable,
							 true,
							 flag_context,
							 GUC_SUPERUSER_ONLY,
							 NULL,
							 NULL,
							 NULL);
}

void
ts_feature_flag_check(FeatureFlagType type)
{
	FeatureFlag *flag = &ts_feature_flags[type];
	if (likely(*flag->enable))
		return;
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("this feature is disabled"),
			 errdetail("Feature flag \"%s\" is off", flag->name)));
}

/*
 * We have to understand if we have finished initializing the GUCs, so that we
 * know when it's OK to check their values for mutual consistency.
 */
static bool gucs_are_initialized = false;

/*
 * Warn about the mismatched cache sizes that can lead to cache thrashing.
 */
static void
validate_chunk_cache_sizes(int hypertable_chunks, int insert_chunks)
{
	/*
	 * Note that this callback is also called when the individual GUCs are
	 * initialized, so we are going to see temporary mismatched values here.
	 * That's why we also have to check that the GUC initialization have
	 * finished.
	 */
	if (gucs_are_initialized && insert_chunks > hypertable_chunks)
	{
		ereport(WARNING,
				(errmsg("insert cache size is larger than hypertable chunk cache size"),
				 errdetail("insert cache size is %d, hypertable chunk cache size is %d",
						   insert_chunks,
						   hypertable_chunks),
				 errhint("This is a configuration problem. Either increase "
						 "timescaledb.max_cached_chunks_per_hypertable (preferred) or decrease "
						 "timescaledb.max_open_chunks_per_insert.")));
	}
}

static void
assign_max_cached_chunks_per_hypertable_hook(int newval, void *extra)
{
	/* invalidate the hypertable cache to reset */
	ts_hypertable_cache_invalidate_callback();

	validate_chunk_cache_sizes(newval, ts_guc_max_open_chunks_per_insert);
}

static void
assign_max_open_chunks_per_insert_hook(int newval, void *extra)
{
	validate_chunk_cache_sizes(ts_guc_max_cached_chunks_per_hypertable, newval);
}

void
_guc_init(void)
{
	DefineCustomBoolVariable("timescaledb.enable_deprecation_warnings",
							 "Enable warnings when using deprecated functionality",
							 NULL,
							 &ts_guc_enable_deprecation_warnings,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_optimizations",
							 "Enable TimescaleDB query optimizations",
							 NULL,
							 &ts_guc_enable_optimizations,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.restoring",
							 "Install timescale in restoring mode",
							 "Used for running pg_restore",
							 &ts_guc_restoring,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_constraint_aware_append",
							 "Enable constraint-aware append scans",
							 "Enable constraint exclusion at execution time",
							 &ts_guc_enable_constraint_aware_append,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_ordered_append",
							 "Enable ordered append scans",
							 "Enable ordered append optimization for queries that are ordered by "
							 "the time dimension",
							 &ts_guc_enable_ordered_append,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_chunk_append",
							 "Enable chunk append node",
							 "Enable using chunk append node",
							 &ts_guc_enable_chunk_append,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_parallel_chunk_append",
							 "Enable parallel chunk append node",
							 "Enable using parallel aware chunk append node",
							 &ts_guc_enable_parallel_chunk_append,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_runtime_exclusion",
							 "Enable runtime chunk exclusion",
							 "Enable runtime chunk exclusion in ChunkAppend node",
							 &ts_guc_enable_runtime_exclusion,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_constraint_exclusion",
							 "Enable constraint exclusion",
							 "Enable planner constraint exclusion",
							 &ts_guc_enable_constraint_exclusion,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_qual_propagation",
							 "Enable qualifier propagation",
							 "Enable propagation of qualifiers in JOINs",
							 &ts_guc_enable_qual_propagation,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_dml_decompression",
							 "Enable DML decompression",
							 "Enable DML decompression when modifying compressed hypertable",
							 &ts_guc_enable_dml_decompression,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_transparent_decompression",
							 "Enable transparent decompression",
							 "Enable transparent decompression when querying hypertable",
							 &ts_guc_enable_transparent_decompression,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_skipscan",
							 "Enable SkipScan",
							 "Enable SkipScan for DISTINCT queries",
							 &ts_guc_enable_skip_scan,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_decompression_logrep_markers",
							 "Enable logical replication markers for decompression ops",
							 "Enable the generation of logical replication markers in the "
							 "WAL stream to mark the start and end of decompressions (for insert, "
							 "update, and delete operations)",
							 &ts_guc_enable_decompression_logrep_markers,
							 false,
							 PGC_SIGHUP,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_decompression_sorted_merge",
							 "Enable compressed batches heap merge",
							 "Enable the merge of compressed batches to preserve the compression "
							 "order by",
							 &ts_guc_enable_decompression_sorted_merge,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_cagg_reorder_groupby",
							 "Enable group by reordering",
							 "Enable group by clause reordering for continuous aggregates",
							 &ts_guc_enable_cagg_reorder_groupby,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_now_constify",
							 "Enable now() constify",
							 "Enable constifying now() in query constraints",
							 &ts_guc_enable_now_constify,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_cagg_watermark_constify",
							 "Enable cagg watermark constify",
							 "Enable constifying cagg watermark for real-time caggs",
							 &ts_guc_enable_cagg_watermark_constify,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_tiered_reads",
							 "Enable tiered data reads",
							 "Enable reading of tiered data by including a foreign table "
							 "representing the data in the object storage into the query plan",
							 &ts_guc_enable_osm_reads,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("timescaledb.max_insert_batch_size",
							"The max number of tuples to batch before sending to a data node",
							"When acting as a access node, TimescaleDB splits batches of "
							"inserted tuples across multiple data nodes. It will batch up to the "
							"configured batch size tuples per data node before flushing. "
							"Setting this to 0 disables batching, reverting to tuple-by-tuple "
							"inserts",
							&ts_guc_max_insert_batch_size,
							1000,
							0,
							65536,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("timescaledb.enable_async_append",
							 "Enable async query execution on data nodes",
							 "Enable optimization that runs remote queries asynchronously"
							 "across data nodes",
							 &ts_guc_enable_async_append,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_chunkwise_aggregation",
							 "Enable chunk-wise aggregation",
							 "Enable the pushdown of aggregations to the"
							 " chunk level",
							 &ts_guc_enable_chunkwise_aggregation,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_vectorized_aggregation",
							 "Enable vectorized aggregation",
							 "Enable vectorized aggregation for compressed data",
							 &ts_guc_enable_vectorized_aggregation,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_compression_indexscan",
							 "Enable compression to take indexscan path",
							 "Enable indexscan during compression, if matching index is found",
							 &ts_guc_enable_compression_indexscan,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_bulk_decompression",
							 "Enable decompression of the entire compressed batches",
							 "Increases throughput of decompression, but might increase query "
							 "memory usage",
							 &ts_guc_enable_bulk_decompression,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("timescaledb.max_open_chunks_per_insert",
							"Maximum open chunks per insert",
							"Maximum number of open chunk tables per insert",
							&ts_guc_max_open_chunks_per_insert,
							1024,
							0,
							PG_INT16_MAX,
							PGC_USERSET,
							0,
							NULL,
							assign_max_open_chunks_per_insert_hook,
							NULL);

	DefineCustomIntVariable("timescaledb.max_cached_chunks_per_hypertable",
							"Maximum cached chunks",
							"Maximum number of chunks stored in the cache",
							&ts_guc_max_cached_chunks_per_hypertable,
							1024,
							0,
							65536,
							PGC_USERSET,
							0,
							NULL,
							assign_max_cached_chunks_per_hypertable_hook,
							NULL);
#ifdef USE_TELEMETRY
	DefineCustomEnumVariable("timescaledb.telemetry_level",
							 "Telemetry settings level",
							 "Level used to determine which telemetry to send",
							 (int *) &ts_guc_telemetry_level,
							 TELEMETRY_DEFAULT,
							 telemetry_level_options,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
#endif

	DefineCustomStringVariable(/* name= */ "timescaledb.license",
							   /* short_desc= */ "TimescaleDB license type",
							   /* long_desc= */ "Determines which features are enabled",
							   /* valueAddr= */ &ts_guc_license,
							   /* bootValue= */ TS_LICENSE_DEFAULT,
							   /* context= */ PGC_SUSET,
							   /* flags= */ 0,
							   /* check_hook= */ ts_license_guc_check_hook,
							   /* assign_hook= */ ts_license_guc_assign_hook,
							   /* show_hook= */ NULL);

	DefineCustomStringVariable(/* name= */ "timescaledb.last_tuned",
							   /* short_desc= */ "last tune run",
							   /* long_desc= */ "records last time timescaledb-tune ran",
							   /* valueAddr= */ &ts_last_tune_time,
							   /* bootValue= */ NULL,
							   /* context= */ PGC_SIGHUP,
							   /* flags= */ 0,
							   /* check_hook= */ NULL,
							   /* assign_hook= */ NULL,
							   /* show_hook= */ NULL);

	DefineCustomStringVariable(/* name= */ "timescaledb.last_tuned_version",
							   /* short_desc= */ "version of timescaledb-tune",
							   /* long_desc= */ "version of timescaledb-tune used to tune",
							   /* valueAddr= */ &ts_last_tune_version,
							   /* bootValue= */ NULL,
							   /* context= */ PGC_SIGHUP,
							   /* flags= */ 0,
							   /* check_hook= */ NULL,
							   /* assign_hook= */ NULL,
							   /* show_hook= */ NULL);

	DefineCustomEnumVariable("timescaledb.bgw_log_level",
							 "Log level for the background worker subsystem",
							 "Log level for the scheduler and workers of the background worker "
							 "subsystem. Requires configuration reload to change.",
							 /* valueAddr= */ &ts_guc_bgw_log_level,
							 /* bootValue= */ WARNING,
							 /* options= */ loglevel_options,
							 /* context= */ PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	/* this information is useful in general on customer deployments */
	DefineCustomBoolVariable(/* name= */ "timescaledb.debug_compression_path_info",
							 /* short_desc= */ "show various compression-related debug info",
							 /* long_desc= */ "this is for debugging/information purposes",
							 /* valueAddr= */ &ts_guc_debug_compression_path_info,
							 /* bootValue= */ false,
							 /* context= */ PGC_USERSET,
							 /* flags= */ 0,
							 /* check_hook= */ NULL,
							 /* assign_hook= */ NULL,
							 /* show_hook= */ NULL);

#ifdef USE_TELEMETRY
	DefineCustomStringVariable(/* name= */ "timescaledb_telemetry.cloud",
							   /* short_desc= */ "cloud provider",
							   /* long_desc= */ "cloud provider used for this instance",
							   /* valueAddr= */ &ts_telemetry_cloud,
							   /* bootValue= */ NULL,
							   /* context= */ PGC_SIGHUP,
							   /* flags= */ 0,
							   /* check_hook= */ NULL,
							   /* assign_hook= */ NULL,
							   /* show_hook= */ NULL);
#endif

#ifdef TS_DEBUG
	DefineCustomBoolVariable(/* name= */ "timescaledb.shutdown_bgw_scheduler",
							 /* short_desc= */ "immediately shutdown the bgw scheduler",
							 /* long_desc= */ "this is for debugging purposes",
							 /* valueAddr= */ &ts_shutdown_bgw,
							 /* bootValue= */ false,
							 /* context= */ PGC_SIGHUP,
							 /* flags= */ 0,
							 /* check_hook= */ NULL,
							 /* assign_hook= */ NULL,
							 /* show_hook= */ NULL);

	DefineCustomStringVariable(/* name= */ "timescaledb.current_timestamp_mock",
							   /* short_desc= */ "set the current timestamp",
							   /* long_desc= */ "this is for debugging purposes",
							   /* valueAddr= */ &ts_current_timestamp_mock,
							   /* bootValue= */ NULL,
							   /* context= */ PGC_USERSET,
							   /* flags= */ 0,
							   /* check_hook= */ NULL,
							   /* assign_hook= */ NULL,
							   /* show_hook= */ NULL);

	DefineCustomEnumVariable(/* name= */ "timescaledb.debug_require_vector_qual",
							 /* short_desc= */
							 "ensure that non-vectorized or vectorized filters are used in "
							 "DecompressChunk node",
							 /* long_desc= */
							 "this is for debugging purposes, to let us check if the vectorized "
							 "quals are used or not. EXPLAIN differs after PG15 for custom nodes, "
							 "and "
							 "using the test templates is a pain",
							 /* valueAddr= */ (int *) &ts_guc_debug_require_vector_qual,
							 /* bootValue= */ RVQ_Allow,
							 /* options = */ require_vector_qual_options,
							 /* context= */ PGC_USERSET,
							 /* flags= */ 0,
							 /* check_hook= */ NULL,
							 /* assign_hook= */ NULL,
							 /* show_hook= */ NULL);

	DefineCustomBoolVariable(/* name= */ "timescaledb.debug_require_batch_sorted_merge",
							 /* short_desc= */ "require batch sorted merge in DecompressChunk node",
							 /* long_desc= */ "this is for debugging purposes",
							 /* valueAddr= */ &ts_guc_debug_require_batch_sorted_merge,
							 /* bootValue= */ false,
							 /* context= */ PGC_USERSET,
							 /* flags= */ 0,
							 /* check_hook= */ NULL,
							 /* assign_hook= */ NULL,
							 /* show_hook= */ NULL);
#endif

	/* register feature flags */
	ts_feature_flag_add(FEATURE_HYPERTABLE);
	ts_feature_flag_add(FEATURE_HYPERTABLE_COMPRESSION);
	ts_feature_flag_add(FEATURE_CAGG);
	ts_feature_flag_add(FEATURE_POLICY);

	gucs_are_initialized = true;

	validate_chunk_cache_sizes(ts_guc_max_cached_chunks_per_hypertable,
							   ts_guc_max_open_chunks_per_insert);
}
