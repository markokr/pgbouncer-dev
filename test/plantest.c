/*
 * Things to test:
 * - Conn per query
 * - show tx
 * - long tx
 * - variable-size query
 */

#include "system.h"

#include <getopt.h>
#include <libpq-fe.h>

int main(int argc, char *argv[])
{
	PGconn *db;
	PGresult *res;
	const char *values[1];

	db = PQconnectdb("dbname=marko port=6000 password=kama");
	if (!db || PQstatus(db) != CONNECTION_OK)
		goto err;

	res = PQexec(db, "begin");
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
		goto err;
	PQclear(res);

	res = PQprepare(db, "myplan", "select $1::text", 1, NULL);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
		goto err;
	PQclear(res);

	values[0] = "hello";
	res = PQexecPrepared(db, "myplan", 1, values, NULL, NULL, 0);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
		goto err;
	PQclear(res);

	res = PQexec(db, "commit");
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
		goto err;
	PQclear(res);

	printf("ok\n");

	return 0;
err:
	printf("error: %s\n", db ? PQerrorMessage(db) : "no mem");
	return 1;
}


