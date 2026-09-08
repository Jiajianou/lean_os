-- tests/sqlite/cases.sql - M100: sqlite, graded against itself.
--
-- Run through the sqlite3 shell twice - once built for the host, once
-- built by this project's compiler and run on the machine - and the two
-- transcripts must be byte-identical. Nothing here says what the right
-- answer is; sqlite on another machine does. tools/build-thirdparty.sh
-- produces the host transcript, and the [m100d] boot self-test cmp's the
-- machine's against it.
--
-- What it is meant to reach: the B-tree on a real file (create, insert
-- a few thousand rows, index, vacuum), the pager's journal (a rolled-back
-- transaction and a committed one, which are two files opened, written,
-- fsync'd and unlinked on this filesystem), the query planner (joins,
-- subqueries, window functions, CTEs), and the library's own arithmetic
-- and string code, which is where a miscompile would show. Everything
-- is deterministic: no now(), no random(), no rowid without ORDER BY.
.mode list
.headers off
SELECT 'version ' || sqlite_version();

CREATE TABLE person(id INTEGER PRIMARY KEY, name TEXT NOT NULL, born INTEGER, city TEXT);
CREATE TABLE city(name TEXT PRIMARY KEY, country TEXT, pop INTEGER);
INSERT INTO city VALUES ('Lisbon','PT',545000),('Porto','PT',231000),('Madrid','ES',3300000),
                        ('Oslo','NO',709000),('Bergen','NO',285000),('Kyoto','JP',1460000);
INSERT INTO person(name, born, city) VALUES
  ('Ada',1815,'Lisbon'),('Grace',1906,'Oslo'),('Edsger',1930,'Madrid'),
  ('Barbara',1936,'Kyoto'),('Dennis',1941,'Porto'),('Ken',1943,'Bergen'),
  ('Margaret',1936,'Oslo'),('Leslie',1941,'Lisbon'),('Frances',1932,NULL);

-- a few thousand rows, so the B-tree has more than one page to split
CREATE TABLE seq(n INTEGER PRIMARY KEY, sq INTEGER, s TEXT);
WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 5000)
  INSERT INTO seq SELECT x, x*x, printf('row-%05d', x) FROM c;
SELECT count(*), sum(sq), min(s), max(s) FROM seq;
CREATE INDEX seq_sq ON seq(sq);
SELECT n FROM seq WHERE sq BETWEEN 2500 AND 2600 ORDER BY n;
SELECT s FROM seq WHERE s LIKE 'row-049%' ORDER BY s DESC LIMIT 3;

-- a transaction rolled back, and one committed, on the real file
BEGIN;
DELETE FROM seq WHERE n % 2 = 0;
SELECT 'inside', count(*) FROM seq;
ROLLBACK;
SELECT 'after rollback', count(*) FROM seq;
BEGIN;
UPDATE seq SET s = upper(s) WHERE n <= 3;
COMMIT;
SELECT s FROM seq WHERE n <= 4 ORDER BY n;

-- joins, grouping, subqueries
SELECT p.name, c.country, c.pop FROM person p JOIN city c ON c.name = p.city
  ORDER BY c.pop DESC, p.name;
SELECT c.country, count(p.id), group_concat(p.name, '+') FROM city c
  LEFT JOIN person p ON p.city = c.name GROUP BY c.country ORDER BY c.country;
SELECT name FROM person WHERE born = (SELECT max(born) FROM person) ORDER BY name;
SELECT name, city FROM person WHERE city IS NULL OR city NOT IN (SELECT name FROM city WHERE country='PT') ORDER BY name;

-- window functions and a CTE
SELECT name, born, rank() OVER (ORDER BY born) AS r,
       sum(born) OVER (ORDER BY born ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) AS running
  FROM person ORDER BY born, name;
WITH old AS (SELECT * FROM person WHERE born < 1935)
  SELECT count(*), avg(born), total(born) FROM old;

-- arithmetic and strings, where a miscompile would show first
SELECT 7/2, 7%3, -7/2, 7.0/2, 1e300*1e10, 9223372036854775807 + 0, -9223372036854775808;
SELECT 0.1+0.2, round(2.5), round(-2.5), round(3.14159, 3), abs(-2147483648), 1<<40, 5 & 3, 5 | 3;
SELECT typeof(1), typeof(1.0), typeof('x'), typeof(x'00ff'), typeof(NULL), hex(x'deadbeef');
SELECT length('héllo'), upper('straße'), substr('abcdef', -3, 2), instr('banana','an'), replace('a-b-c','-','+');
SELECT printf('%5.2f|%-6s|%08x|%e', 3.14159, 'ab', 48879, 12345.678);
SELECT quote('it''s'), trim('  x  '), ltrim('xxay','x'), zeroblob(3), unicode('€'), char(72,105);
SELECT date('2024-02-29','+1 year'), datetime('2000-01-01 12:00:00','-90 minutes'),
       strftime('%j %W %s', '1999-12-31'), julianday('2000-01-01');
SELECT json_extract('{"a":[1,2,{"b":3}]}', '$.a[2].b'), json_array(1,'two',null), json_valid('{');
SELECT CAST('12abc' AS INTEGER), CAST(3.99 AS INTEGER), CAST(1e20 AS INTEGER), 'a' < 'B', 'a' < 'B' COLLATE NOCASE;

-- the planner, told to explain itself
EXPLAIN QUERY PLAN SELECT p.name FROM person p JOIN city c ON c.name = p.city WHERE c.country = 'NO';
EXPLAIN QUERY PLAN SELECT n FROM seq WHERE sq = 49;

-- the shell's own popen and system, which this libc grew for it
.system echo system-ok
CREATE TABLE piped(n INTEGER, word TEXT);
.import '|printf "7,seven\n8,eight\n"' piped --csv
SELECT n, word FROM piped ORDER BY n;

-- the file itself
PRAGMA page_size;
PRAGMA integrity_check;
DELETE FROM seq WHERE n > 100;
VACUUM;
PRAGMA integrity_check;
PRAGMA page_count;
SELECT count(*) FROM seq;
SELECT name, type FROM sqlite_master ORDER BY name;
DROP TABLE seq;
SELECT count(*) FROM sqlite_master;
SELECT 'sqlite: done';
