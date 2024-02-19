-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

create table readings(time timestamptz,
       location int,
       device int,
       temp float,
       humidity float,
       unique (device, location, time)
);

create index on readings(location);
create index on readings(device);

select create_hypertable('readings', 'time');

select setseed(1);

insert into readings (time, location, device, temp, humidity)
select t, ceil(random()*10), ceil(random()*30), random()*40, random()*100
from generate_series('2022-06-01'::timestamptz, '2022-07-01', '5s') t;

alter table readings set (
      timescaledb.compress,
      timescaledb.compress_orderby = 'time',
      timescaledb.compress_segmentby = 'device'
);

select format('%I.%I', chunk_schema, chunk_name)::regclass as chunk
  from timescaledb_information.chunks
 where format('%I.%I', hypertable_schema, hypertable_name)::regclass = 'readings'::regclass
 limit 1 \gset

alter table :chunk set access method hyperstore;

--
-- Check that TID scan works for both compressed and non-compressed
-- rows.
--

set timescaledb.enable_transparent_decompression to false;

-- Select any row and try to fetch it using CTID. We do not select the
-- first one just to also try to scan a few rows and make sure the
-- implementation works.
select ctid from :chunk limit 1 offset 10 \gset

explain (analyze, costs off, timing off, summary off, decompress_cache_stats)
select * from :chunk where ctid = :'ctid';
select * from :chunk where ctid = :'ctid';

-- Insert a new row, which will then be non-compressed, and fetch it.
insert into :chunk values ('Wed May 25 17:34:56 2022 PDT', 1, 2, 3.14, 2.14);
select ctid from :chunk where time = 'Wed May 25 17:34:56 2022 PDT' \gset

explain (analyze, costs off, timing off, summary off, decompress_cache_stats)
select * from :chunk where ctid = :'ctid';
select * from :chunk where ctid = :'ctid';

-- Check that a bad option name generates an error.
\set ON_ERROR_STOP 0
explain (analyze, costs off, timing off, summary off, decopress_cache_stats)
select * from :chunk where device between 5 and 10;
\set ON_ERROR_STOP 1

explain (analyze, costs off, timing off, summary off, decompress_cache_stats)
select time, temp + humidity from readings where device between 5 and 10 and humidity > 5;

-- Check the explain cache information output.
--
-- Query 1 and 3 should show the same explain plan, and the plan in
-- the middle should not include decompress stats:
explain (analyze, costs off, timing off, summary off, decompress_cache_stats)
select time, temp + humidity from readings where device between 5 and 10 and humidity > 5;

-- Check the explain cache information output. Query 1 and 3 should
-- show the same explain plan, and the plan in the middle should not
-- include decompress stats:
explain (analyze, costs off, timing off, summary off, decompress_cache_stats)
select * from :chunk where device between 5 and 10;
explain (analyze, costs off, timing off, summary off, decompress_cache_stats)
select * from :chunk where device between 5 and 10;

-- Queries that will select just a few columns
set max_parallel_workers_per_gather to 0;
explain (analyze, costs off, timing off, summary off, decompress_cache_stats)
select device, humidity from readings where device between 5 and 10;

explain (analyze, costs off, timing off, summary off, decompress_cache_stats)
select device, avg(humidity) from readings where device between 5 and 10
group by device;

-- Test on conflict: insert the same data as before, but throw away
-- the updates.
explain (analyze, costs off, timing off, summary off, decompress_cache_stats)
insert into readings (time, location, device, temp, humidity)
select t, ceil(random()*10), ceil(random()*30), random()*40, random()*100
from generate_series('2022-06-01'::timestamptz, '2022-07-01', '5s') t
on conflict (location, device, time) do nothing;

-- This should show values for all columns
explain (analyze, costs off, timing off, summary off, decompress_cache_stats)
select time, temp + humidity from readings where device between 5 and 10 and humidity > 5 limit 5;
select time, temp + humidity from readings where device between 5 and 10 and humidity > 5 limit 5;