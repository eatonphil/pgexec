# pgexec: Messing with Postgres query execution and hook infrastructure

Build and install debug Postgres from source:

```console
$ git clone https://github.com/postgres/postgres
$ ./configure --enable-cassert --enable-debug CFLAGS="-ggdb -Og -g3 -fno-omit-frame-pointer"
$ make -j8
$ sudo make install # Assume stuff has been installed to /usr/local/pgsql/bin
```

Install this extension:

```console
$ make && sudo make install
```

Make a new database and add `pgexec` to `shared_preload_libraries` in
`postgresql.conf`, then start Postgres:

```console
$ /usr/local/pgsql/bin/initdb test-db
$ # Modify test-db/postgresql.conf to set `shared_preload_libraries = 'pgexec'`.
$ /usr/local/pgsql/bin/postgres --config-file=$(pwd)/test-db/postgresql.conf -D $(pwd)/test-db -k $(pwd)/test-db/
2023-11-15 20:10:03.461 UTC [3144087] LOG:  starting PostgreSQL 17devel on x86_64-pc-linux-gnu, compiled by gcc (GCC) 13.2.1 20230728 (Red Hat 13.2.1-1), 64-bit
2023-11-15 20:10:03.461 UTC [3144087] LOG:  listening on IPv6 address "::1", port 5432
2023-11-15 20:10:03.461 UTC [3144087] LOG:  listening on IPv4 address "127.0.0.1", port 5432
2023-11-15 20:10:03.462 UTC [3144087] LOG:  listening on Unix socket "/home/phil/projects/pgexec/test-db/.s.PGSQL.5432"
2023-11-15 20:10:03.463 UTC [3144090] LOG:  database system was shut down at 2023-11-15 20:09:45 UTC
2023-11-15 20:10:03.465 UTC [3144087] LOG:  database system is ready to accept connections
```

In a new window/tab/whatever, run `test.sql`:

```console
$ cat test.sql
DROP TABLE IF EXISTS x;
CREATE TABLE x(a INT);
INSERT INTO x VALUES (23), (101); -- This is ignored at the moment.
SELECT a FROM x;
SELECT a FROM x WHERE a = 1; -- But filtering works.
$ /usr/local/pgsql/bin/psql -h localhost postgres -f test.sql
/usr/local/pgsql/bin/psql -h localhost postgres -f test.sql
DROP TABLE
CREATE TABLE
INSERT 0 2
 a
---
 0
 1
(2 rows)

 a
---
 1
(1 row)
```
