DROP TABLE IF EXISTS x;
CREATE TABLE x(a INT);
INSERT INTO x VALUES (23), (101); -- This is ignored at the moment.
SELECT
  a FROM
      x;
SELECT          a
  FROM
    x WHERE
  a
= 1; -- But filtering works.
