-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

\set TEST_BASE_NAME cagg_query
SELECT
       format('%s/results/%s_results_view.out', :'TEST_OUTPUT_DIR', :'TEST_BASE_NAME') as "TEST_RESULTS_VIEW",
       format('%s/results/%s_results_view_hashagg.out', :'TEST_OUTPUT_DIR', :'TEST_BASE_NAME') as "TEST_RESULTS_VIEW_HASHAGG",
       format('%s/results/%s_results_table.out', :'TEST_OUTPUT_DIR', :'TEST_BASE_NAME') as "TEST_RESULTS_TABLE"
\gset
SELECT format('\! diff %s %s', :'TEST_RESULTS_VIEW', :'TEST_RESULTS_TABLE') as "DIFF_CMD",
      format('\! diff %s %s', :'TEST_RESULTS_VIEW_HASHAGG', :'TEST_RESULTS_TABLE') as "DIFF_CMD2"
\gset


\set EXPLAIN 'EXPLAIN (VERBOSE, COSTS OFF)'

SET client_min_messages TO NOTICE;

CREATE TABLE conditions (
      timec        TIMESTAMPTZ       NOT NULL,
      location    TEXT              NOT NULL,
      temperature DOUBLE PRECISION  NULL,
      humidity    DOUBLE PRECISION  NULL
    );

select table_name from create_hypertable( 'conditions', 'timec');

insert into conditions values ( '2018-01-01 09:20:00-08', 'SFO', 55, 45);
insert into conditions values ( '2018-01-02 09:30:00-08', 'por', 100, 100);
insert into conditions values ( '2018-01-02 09:20:00-08', 'SFO', 65, 45);
insert into conditions values ( '2018-01-02 09:10:00-08', 'NYC', 65, 45);
insert into conditions values ( '2018-11-01 09:20:00-08', 'NYC', 45, 30);
insert into conditions values ( '2018-11-01 10:40:00-08', 'NYC', 55, 35);
insert into conditions values ( '2018-11-01 11:50:00-08', 'NYC', 65, 40);
insert into conditions values ( '2018-11-01 12:10:00-08', 'NYC', 75, 45);
insert into conditions values ( '2018-11-01 13:10:00-08', 'NYC', 85, 50);
insert into conditions values ( '2018-11-02 09:20:00-08', 'NYC', 10, 10);
insert into conditions values ( '2018-11-02 10:30:00-08', 'NYC', 20, 15);
insert into conditions values ( '2018-11-02 11:40:00-08', 'NYC', null, null);
insert into conditions values ( '2018-11-03 09:50:00-08', 'NYC', null, null);

create table location_tab( locid integer, locname text );
insert into location_tab values( 1, 'SFO');
insert into location_tab values( 2, 'NYC');
insert into location_tab values( 3, 'por');

create materialized view mat_m1( location, timec, minl, sumt , sumh)
WITH (timescaledb.continuous, timescaledb.materialized_only=false)
as
select location, time_bucket('1day', timec), min(location), sum(temperature),sum(humidity)
from conditions
group by time_bucket('1day', timec), location WITH NO DATA;

--compute time_bucketted max+bucket_width for the materialized view
SELECT time_bucket('1day' , q.timeval+ '1day'::interval)
FROM ( select max(timec)as timeval from conditions ) as q;
CALL refresh_continuous_aggregate('mat_m1', NULL, NULL);

--test first/last
create materialized view mat_m2(location, timec, firsth, lasth, maxtemp, mintemp)
WITH (timescaledb.continuous, timescaledb.materialized_only=false)
as
select location, time_bucket('1day', timec), first(humidity, timec), last(humidity, timec), max(temperature), min(temperature)
from conditions
group by time_bucket('1day', timec), location WITH NO DATA;
--time that refresh assumes as now() for repeatability
SELECT time_bucket('1day' , q.timeval+ '1day'::interval)
FROM ( select max(timec)as timeval from conditions ) as q;
CALL refresh_continuous_aggregate('mat_m2', NULL, NULL);

--normal view --
create or replace view regview( location, timec, minl, sumt , sumh)
as
select location, time_bucket('1day', timec), min(location), sum(temperature),sum(humidity)
from conditions
group by location, time_bucket('1day', timec);

set enable_hashagg = false;

-- NO pushdown cases ---
--when we have addl. attrs in order by that are not in the
-- group by, we will still need a sort
:EXPLAIN
select * from mat_m1 order by sumh, sumt, minl, timec ;
:EXPLAIN
select * from regview order by timec desc;

-- PUSHDOWN cases --
-- all group by elts in order by , reorder group by elts to match
-- group by order
-- This should prevent an additional sort after GroupAggregate
:EXPLAIN
select * from mat_m1 order by timec desc, location;

:EXPLAIN
select * from mat_m1 order by location, timec desc;

:EXPLAIN
select * from mat_m1 order by location, timec asc;
:EXPLAIN
select * from mat_m1 where timec > '2018-10-01' order by timec desc;
-- outer sort is used by mat_m1 for grouping. But doesn't avoid a sort after the join ---
:EXPLAIN
select l.locid, mat_m1.* from mat_m1 , location_tab l where timec > '2018-10-01' and l.locname = mat_m1.location order by timec desc;

:EXPLAIN
select * from mat_m2 where timec > '2018-10-01' order by timec desc;

:EXPLAIN
select * from (select * from mat_m2 where timec > '2018-10-01' order by timec desc ) as q limit 1;

:EXPLAIN
select * from (select * from mat_m2 where timec > '2018-10-01' order by timec desc , location asc nulls first) as q limit 1;

--plans with CTE
:EXPLAIN
with m1 as (
Select * from mat_m2 where timec > '2018-10-01' order by timec desc )
select * from m1;

-- should reorder mat_m1 group by only based on mat_m1 order-by
:EXPLAIN
select * from mat_m1, mat_m2 where mat_m1.timec > '2018-10-01' and mat_m1.timec = mat_m2.timec order by mat_m1.timec desc;
--should reorder only for mat_m1.
:EXPLAIN
select * from mat_m1, regview where mat_m1.timec > '2018-10-01' and mat_m1.timec = regview.timec order by mat_m1.timec desc;

select l.locid, mat_m1.* from mat_m1 , location_tab l where timec > '2018-10-01' and l.locname = mat_m1.location order by timec desc;

\set ECHO none
SET client_min_messages TO error;
\o :TEST_RESULTS_VIEW
select * from mat_m1 order by timec desc, location;
select * from mat_m1 order by location, timec desc;
select * from mat_m1 order by location, timec asc;
select * from mat_m1 where timec > '2018-10-01' order by timec desc;
select * from mat_m2 where timec > '2018-10-01' order by timec desc;
\o
RESET client_min_messages;
\set ECHO all

---- Run the same queries with hash agg enabled now
set enable_hashagg = true;
\set ECHO none
SET client_min_messages TO error;
\o :TEST_RESULTS_VIEW_HASHAGG
select * from mat_m1 order by timec desc, location;
select * from mat_m1 order by location, timec desc;
select * from mat_m1 order by location, timec asc;
select * from mat_m1 where timec > '2018-10-01' order by timec desc;
select * from mat_m2 where timec > '2018-10-01' order by timec desc;
\o
RESET client_min_messages;
\set ECHO all

--- Run the queries directly on the table now
set enable_hashagg = true;
\set ECHO none
SET client_min_messages TO error;
\o :TEST_RESULTS_TABLE
SELECT location, time_bucket('1day', timec) as timec, min(location) as minl, sum(temperature) as sumt, sum(humidity) as sumh from conditions group by time_bucket('1day', timec) , location
order by timec desc, location;
SELECT location, time_bucket('1day', timec) as timec, min(location) as minl, sum(temperature) as sumt, sum(humidity) as sumh from conditions group by time_bucket('1day', timec) , location
order by location, timec desc;
SELECT location, time_bucket('1day', timec) as timec, min(location) as minl, sum(temperature) as sumt, sum(humidity) as sumh from conditions group by time_bucket('1day', timec) , location
order by location, timec asc;
select * from (SELECT location, time_bucket('1day', timec) as timec, min(location) as minl, sum(temperature) as sumt, sum(humidity) as sumh from conditions
group by time_bucket('1day', timec) , location ) as q
where timec > '2018-10-01' order by timec desc;
--comparison for mat_m2 queries
select * from (
select location, time_bucket('1day', timec) as timec, first(humidity, timec) firsth, last(humidity, timec) lasth, max(temperature) maxtemp, min(temperature) mintemp
from conditions
group by time_bucket('1day', timec), location) as q
where timec > '2018-10-01' order by timec desc limit 10;
\o
RESET client_min_messages;
\set ECHO all

-- diff results view select and table select
:DIFF_CMD
:DIFF_CMD2

--check if the guc works , reordering will not work
set timescaledb.enable_cagg_reorder_groupby = false;
set enable_hashagg = false;
:EXPLAIN
select * from mat_m1 order by timec desc, location;

-----------------------------------------------------------------------
-- Test the cagg_watermark function. The watermark gives the point
-- where to UNION raw and materialized data in real-time
-- aggregation. Specifically, test that the watermark caching works as
-- expected.
-----------------------------------------------------------------------

-- Insert some more data so that there is something to UNION in
-- real-time aggregation.

insert into conditions values ( '2018-12-02 20:10:00-08', 'SFO', 55, 45);
insert into conditions values ( '2018-12-02 21:20:00-08', 'SFO', 65, 45);
insert into conditions values ( '2018-12-02 20:30:00-08', 'NYC', 65, 45);
insert into conditions values ( '2018-12-02 21:50:00-08', 'NYC', 45, 30);

-- Test join of two caggs. Joining two caggs will force the cache to
-- reset every time the watermark function is invoked on a different
-- cagg in the same query.
SELECT mat_hypertable_id AS mat_id,
	   raw_hypertable_id AS raw_id,
	   schema_name AS mat_schema,
	   table_name AS mat_name,
	   format('%I.%I', schema_name, table_name) AS mat_table
FROM _timescaledb_catalog.continuous_agg ca, _timescaledb_catalog.hypertable h
WHERE user_view_name='mat_m1'
AND h.id = ca.mat_hypertable_id \gset

BEGIN;

-- Query without join
SELECT m1.location, m1.timec, sumt, sumh
FROM mat_m1 m1
ORDER BY m1.location COLLATE "C", m1.timec DESC
LIMIT 10;

-- Query that joins two caggs. This should force the watermark cache
-- to reset when the materialized hypertable ID changes. A hash join
-- could potentially read all values from mat_m1 then all values from
-- mat_m2. This would be the optimal situation for cagg_watermark
-- caching. We want to avoid it in tests to see that caching doesn't
-- do anything wrong in worse situations (e.g., a nested loop join).
SET enable_hashjoin=false;

SELECT m1.location, m1.timec, sumt, sumh, firsth, lasth, maxtemp, mintemp
FROM mat_m1 m1 RIGHT JOIN mat_m2 m2
ON (m1.location = m2.location
AND m1.timec = m2.timec)
ORDER BY m1.location COLLATE "C", m1.timec DESC
LIMIT 10;

-- Show the current watermark
SELECT _timescaledb_functions.to_timestamp(_timescaledb_functions.cagg_watermark(:mat_id));

-- The watermark should, in this case, be the same as the invalidation
-- threshold
SELECT _timescaledb_functions.to_timestamp(watermark)
FROM _timescaledb_catalog.continuous_aggs_invalidation_threshold
WHERE hypertable_id = :raw_id;

-- The watermark is the end of materialization (end of last bucket)
-- while the MAX is the start of the last bucket
SELECT max(timec) FROM :mat_table;

-- Drop the most recent chunk
SELECT chunk_name, range_start, range_end
FROM timescaledb_information.chunks
WHERE hypertable_name = :'mat_name';

SELECT drop_chunks('mat_m1', newer_than=>'2018-01-01'::timestamptz);

SELECT chunk_name, range_start, range_end
FROM timescaledb_information.chunks
WHERE hypertable_name = :'mat_name';

-- The watermark should be updated to reflect the dropped data (i.e.,
-- the cache should be reset)
SELECT _timescaledb_functions.to_timestamp(_timescaledb_functions.cagg_watermark(:mat_id));

-- Since we removed the last chunk, the invalidation threshold doesn't
-- move back, while the watermark does.
SELECT _timescaledb_functions.to_timestamp(watermark)
FROM _timescaledb_catalog.continuous_aggs_invalidation_threshold
WHERE hypertable_id = :raw_id;

-- Compare the new watermark to the MAX time in the table
SELECT max(timec) FROM :mat_table;

-- Try a subtransaction
SAVEPOINT clear_cagg;

SELECT m1.location, m1.timec, sumt, sumh, firsth, lasth, maxtemp, mintemp
FROM mat_m1 m1 RIGHT JOIN mat_m2 m2
ON (m1.location = m2.location
AND m1.timec = m2.timec)
ORDER BY m1.location COLLATE "C", m1.timec DESC
LIMIT 10;

ALTER MATERIALIZED VIEW mat_m1 SET (timescaledb.materialized_only=true);

SELECT m1.location, m1.timec, sumt, sumh, firsth, lasth, maxtemp, mintemp
FROM mat_m1 m1 RIGHT JOIN mat_m2 m2
ON (m1.location = m2.location
AND m1.timec = m2.timec)
ORDER BY m1.location COLLATE "C" NULLS LAST, m1.timec DESC NULLS LAST, firsth NULLS LAST,
         lasth NULLS LAST, mintemp NULLS LAST, maxtemp NULLS LAST
LIMIT 10;

ROLLBACK;
