-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

-- Make a compressed table with a compressed string of varying length, to test
-- the various ways the compressed data can be toasted.
create table longstr(ts int default 1, s1 text);
select create_hypertable('longstr', 'ts');
alter table longstr set (timescaledb.compress);


-- We want to test the case for inline compression. It is technically possible,
-- but very hard to hit with the usual toast_tuple_target = 128 on compressed
-- chunks. So here we increase the toast_tuple_target to simplify
-- testing.
select format('%I.%I', schema_name, table_name) compressed_table
from _timescaledb_catalog.hypertable
where id = (select compressed_hypertable_id from _timescaledb_catalog.hypertable
    where table_name = 'longstr')
\gset
alter table :compressed_table set (toast_tuple_target = 512);


-- Now, test compression and decompression with various string lengths.
create function test(repeats int) returns table(ns bigint) as $$ begin
    raise log 'repeats %', repeats;
    truncate longstr;
    insert into longstr(s1) select repeat('aaaa', repeats);
    perform count(compress_chunk(x, true)) from show_chunks('longstr') x;
    return query select sum(length(s1)) from longstr;
end; $$ language plpgsql volatile;

select sum(t) from generate_series(1, 30) x, lateral test(x * x * x) t;
