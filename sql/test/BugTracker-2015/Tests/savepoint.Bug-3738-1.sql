START TRANSACTION;
CREATE TABLE foo (a int);
SAVEPOINT sp;
RELEASE SAVEPOINT sp;
COMMIT;
SELECT * FROM foo;
