drop table if exists x;
create table x(a int);
INSERT INTO x VALUES (23), (101);
select a from x;
select a from x where a = 1;
