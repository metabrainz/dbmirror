/****************************************************************************
 * pending.c
 * $Id: pending.c,v 1.2 2006/09/16 17:46:50 ssinger Exp $
 * $PostgreSQL: pgsql/contrib/dbmirror/pending.c,v 1.26 2006/07/11 17:26:58 momjian Exp $
 *
 * This file contains a trigger for Postgresql-7.x to record changes to tables
 * to a pending table for mirroring.
 * All tables that should be mirrored should have this trigger hooked up to it.
 *
 *	 Written by Steven Singer
 *	 (c) 2001-2002 Navtech Systems Support Inc.
 *		 ALL RIGHTS RESERVED
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 *
 ***************************************************************************/
#include "postgres.h"

#include "executor/spi.h"

#include "commands/sequence.h"
#include "commands/trigger.h"
#include "utils/lsyscache.h"
#include "utils/array.h"
#include "utils/rel.h"
#include "catalog/pg_type.h"
#include "access/xact.h"

PG_MODULE_MAGIC;

enum FieldUsage
{
	PRIMARY = 0, NONPRIMARY, ALLKEYS, ALL, NUM_FIELDUSAGE
};

int storePending(char *cpTableName, HeapTuple tBeforeTuple,
			 HeapTuple tAfterTuple,
			 TupleDesc tTupdesc,
			 Oid tableOid,
			 char cOp,
			 bool verbose);



int storeKeyInfo(char *cpTableName, HeapTuple tTupleData, TupleDesc tTuplDesc,
			 Oid tableOid);
int storeData(char *cpTableName, HeapTuple tTupleData,
		  TupleDesc tTupleDesc, Oid tableOid, bool isKey, enum FieldUsage eKeyUsage);

int2vector *getPrimaryKey(Oid tblOid);
ArrayType *getForeignKey(Oid tblOid);

char *packageData(HeapTuple tTupleData, TupleDesc tTupleDecs, Oid tableOid,
			enum FieldUsage eKeyUsage);


#define BUFFER_SIZE 256
#define MAX_OID_LEN 10
/*#define DEBUG_OUTPUT 1 */
extern Datum recordchange(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(recordchange);


#if defined DEBUG_OUTPUT
#define debug_msg2(x,y) elog(NOTICE,x,y)
#define debug_msg(x) elog(NOTICE,x)
#define debug_msg3(x,y,z) elog(NOTICE,x,y,z)
#else
#define debug_msg2(x,y)
#define debug_msg(x)
#define debug_msg3(x,y,z)
#endif



extern Datum setval_mirror(PG_FUNCTION_ARGS);
extern Datum setval3_mirror(PG_FUNCTION_ARGS);
extern Datum nextval_mirror(PG_FUNCTION_ARGS);

static void saveSequenceUpdate(Oid relid, int64 nextValue, bool iscalled);


/*****************************************************************************
 * The entry point for the trigger function.
 * The Trigger takes a single SQL 'text' argument indicating the name of the
 * table the trigger was applied to.  If this name is incorrect so will the
 * mirroring.
 ****************************************************************************/
Datum
recordchange(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata;
	Trigger		*trigger;
	TupleDesc	tupdesc;
	HeapTuple	beforeTuple = NULL;
	HeapTuple	afterTuple = NULL;
	HeapTuple	retTuple = NULL;
	char	   *tblname;
	char		op = 0;
	char	   *schemaname;
	char	   *fullyqualtblname;
	char	   *pkxpress = NULL;
	bool	    verbose;

	if (fcinfo->context != NULL)
	{

		if (SPI_connect() < 0)
		{
			ereport(ERROR, (errcode(ERRCODE_CONNECTION_FAILURE),
				  errmsg("dbmirror:recordchange could not connect to SPI")));
			return -1;
		}

		trigdata = (TriggerData *) fcinfo->context;

		/* Get parameter to known if verbose mode should be activated (off by default) */
		trigger = trigdata->tg_trigger;
		if (trigger->tgnargs < 1) {
			verbose = FALSE;
		} else {
			verbose = (strcmp(trigger->tgargs[0], "verbose") == 0) ? TRUE : FALSE;
		}
		debug_msg2("dbmirror:recordchange verbose mode = %i", verbose);

		/* Extract the table name */
		tblname = SPI_getrelname(trigdata->tg_relation);
#ifndef NOSCHEMAS
		schemaname = get_namespace_name(RelationGetNamespace(trigdata->tg_relation));
		fullyqualtblname = SPI_palloc(strlen(tblname) +
									  strlen(schemaname) + 6);
		sprintf(fullyqualtblname, "\"%s\".\"%s\"",
				schemaname, tblname);
#else
		fullyqualtblname = SPI_palloc(strlen(tblname) + 3);
		sprintf(fullyqualtblname, "\"%s\"", tblname);
#endif
		tupdesc = trigdata->tg_relation->rd_att;
		if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		{
			retTuple = trigdata->tg_newtuple;
			beforeTuple = trigdata->tg_trigtuple;
			afterTuple = trigdata->tg_newtuple;
			op = 'u';

		}
		else if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		{
			retTuple = trigdata->tg_trigtuple;
			afterTuple = trigdata->tg_trigtuple;
			op = 'i';
		}
		else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
		{
			retTuple = trigdata->tg_trigtuple;
			beforeTuple = trigdata->tg_trigtuple;
			op = 'd';
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_TRIGGERED_ACTION_EXCEPTION),
						 errmsg("dbmirror:recordchange Unknown operation")));

		}

		if (storePending(fullyqualtblname, beforeTuple, afterTuple,
						 tupdesc, retTuple->t_tableOid, op, verbose))
		{
			/* An error occoured. Skip the operation. */
			ereport(ERROR,
					(errcode(ERRCODE_TRIGGERED_ACTION_EXCEPTION),
					 errmsg("operation could not be mirrored")));

			return PointerGetDatum(NULL);

		}
		debug_msg("dbmirror:recordchange returning on success");

		SPI_pfree(fullyqualtblname);
		if (pkxpress != NULL)
			SPI_pfree(pkxpress);
		SPI_finish();
		return PointerGetDatum(retTuple);
	}
	else
	{
		/*
		 * Not being called as a trigger.
		 */
		return PointerGetDatum(NULL);
	}
}


/*****************************************************************************
 * Constructs and executes an SQL query to write a record of this tuple change
 * to the pending table.
 *****************************************************************************/
int
storePending(char *cpTableName, HeapTuple tBeforeTuple,
			 HeapTuple tAfterTuple,
			 TupleDesc tTupDesc,
			 Oid tableOid,
			 char cOp,
			 bool verbose)
{
	char	   *cpQueryBase = "INSERT INTO dbmirror_pending (TableName,Op,XID) VALUES ($1,$2,$3)";

	int			iResult = 0;
	HeapTuple	tCurTuple;
	char		nulls[3] = "   ";

	/* Points the current tuple(before or after) */
	Datum		saPlanData[3];
	Oid			taPlanArgTypes[4] = {NAMEOID,
		CHAROID,
	INT4OID};
	void	   *vpPlan;

	tCurTuple = tBeforeTuple ? tBeforeTuple : tAfterTuple;



	vpPlan = SPI_prepare(cpQueryBase, 3, taPlanArgTypes);
	if (vpPlan == NULL)
		ereport(ERROR, (errcode(ERRCODE_TRIGGERED_ACTION_EXCEPTION),
						errmsg("dbmirror:storePending error creating plan")));


	saPlanData[0] = PointerGetDatum(cpTableName);
	saPlanData[1] = CharGetDatum(cOp);
	saPlanData[2] = Int32GetDatum(GetCurrentTransactionId());

	iResult = SPI_execp(vpPlan, saPlanData, nulls, 1);
	if (iResult < 0)
		elog(NOTICE, "storedPending fired (%s) returned %d",
			 cpQueryBase, iResult);



	debug_msg("dbmirror:storePending row successfully stored in pending table");


	if (cOp == 'd')
	{
		/**
		 * This is a record of a delete operation.
		 * Store the key data, or all data, functions of the verbose parameter.
		 */
		if (verbose == TRUE)
			iResult = storeData(cpTableName, tBeforeTuple, tTupDesc, tableOid, TRUE, ALLKEYS);
		else
			iResult = storeKeyInfo(cpTableName, tBeforeTuple, tTupDesc, tableOid);

	}
	else if (cOp == 'i')
	{
		/**
		 * An Insert operation.
		 * Store all data
		 */
		iResult = storeData(cpTableName, tAfterTuple, tTupDesc, tableOid, FALSE, ALL);

	}
	else
	{
		/* op must be an update. */
		if (verbose == TRUE)
			iResult = storeData(cpTableName, tBeforeTuple, tTupDesc, tableOid, TRUE, ALLKEYS);
		else
			iResult = storeKeyInfo(cpTableName, tBeforeTuple, tTupDesc, tableOid);

		iResult = iResult ? iResult :
			storeData(cpTableName, tAfterTuple, tTupDesc, tableOid, FALSE, ALL);
	}


	debug_msg("dbmirror:storePending done storing keyinfo");

	return iResult;

}

int
storeKeyInfo(char *cpTableName, HeapTuple tTupleData,
			 TupleDesc tTupleDesc, Oid tableOid)
{

	Oid			saPlanArgTypes[1] = {VARCHAROID};
	char	   *insQuery = "INSERT INTO dbmirror_pendingdata (SeqId,IsKey,Data) VALUES(currval('dbmirror_pending_seqid_seq'),'t',$1)";
	void	   *pplan;
	Datum		saPlanData[1];
	char	   *cpKeyData;
	int			iRetCode;

	pplan = SPI_prepare(insQuery, 1, saPlanArgTypes);
	if (pplan == NULL)
	{
		elog(NOTICE, "could not prepare INSERT plan");
		return -1;
	}

	/* pplan = SPI_saveplan(pplan); */
	cpKeyData = packageData(tTupleData, tTupleDesc, tableOid, PRIMARY);
	if (cpKeyData == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
		/* cpTableName already contains quotes... */
				 errmsg("there is no PRIMARY KEY for table %s",
						cpTableName)));


	debug_msg2("dbmirror:storeKeyInfo key data: %s", cpKeyData);

	saPlanData[0] = PointerGetDatum(cpKeyData);

	iRetCode = SPI_execp(pplan, saPlanData, NULL, 1);

	if (cpKeyData != NULL)
		SPI_pfree(cpKeyData);

	if (iRetCode != SPI_OK_INSERT)
		ereport(ERROR,
				(errcode(ERRCODE_TRIGGERED_ACTION_EXCEPTION),
				 errmsg("error inserting row in pendingDelete")));

	debug_msg("insert successful");

	return 0;

}




int2vector *
getPrimaryKey(Oid tblOid)
{
	char	   *queryBase;
	char	   *query;
	bool		isNull;
	int2vector *resultKey;
	int2vector *tpResultKey;
	HeapTuple	resTuple;
	Datum		resDatum;
	int			ret;

	queryBase = "SELECT indkey FROM pg_index WHERE indisprimary='t' AND indrelid=";
	query = SPI_palloc(strlen(queryBase) + MAX_OID_LEN + 1);
	sprintf(query, "%s%d", queryBase, tblOid);
	ret = SPI_exec(query, 1);
	SPI_pfree(query);
	if (ret != SPI_OK_SELECT || SPI_processed != 1)
		return NULL;

	resTuple = SPI_tuptable->vals[0];
	resDatum = SPI_getbinval(resTuple, SPI_tuptable->tupdesc, 1, &isNull);

	tpResultKey = (int2vector *) DatumGetPointer(resDatum);
	resultKey = SPI_palloc(VARSIZE(tpResultKey));
	memcpy(resultKey, tpResultKey, VARSIZE(tpResultKey));

	return resultKey;
}

ArrayType *
getForeignKey(Oid tblOid)
{
	char	   *queryBase;
	char	   *query;
	bool		isNull;
	ArrayType  *resultKey;
	HeapTuple	resTuple;
	Datum		resDatum;
	int			ret;

	queryBase = "SELECT array_cat_agg(conkey) FROM pg_constraint WHERE contype = 'f' AND conrelid=";
	query = SPI_palloc(strlen(queryBase) + MAX_OID_LEN + 1);
	sprintf(query, "%s%d", queryBase, tblOid);
	ret = SPI_exec(query, 1);
	SPI_pfree(query);
	if (ret != SPI_OK_SELECT || SPI_processed != 1)
		return NULL;

	resTuple = SPI_tuptable->vals[0];
	resDatum = SPI_getbinval(resTuple, SPI_tuptable->tupdesc, 1, &isNull);

	resultKey = DatumGetArrayTypePCopy(resDatum);

	return resultKey;

}

/******************************************************************************
 * Stores a copy of the non-key data for the row.
 *****************************************************************************/
int
storeData(char *cpTableName, HeapTuple tTupleData,
		  TupleDesc tTupleDesc, Oid tableOid, bool isKey, enum FieldUsage eKeyUsage)
{

	Oid			planArgTypes[2] = {BOOLOID, VARCHAROID};
	char	   *insQuery = "INSERT INTO dbmirror_pendingdata (SeqId,IsKey,Data) VALUES(currval('dbmirror_pending_seqid_seq'),$1,$2)";
	void	   *pplan;
	Datum		planData[2];
	char	   *cpKeyData;
	int			iRetValue;

	pplan = SPI_prepare(insQuery, 2, planArgTypes);
	if (pplan == NULL)
	{
		elog(NOTICE, "could not prepare INSERT plan");
		return -1;
	}

	/* pplan = SPI_saveplan(pplan); */
	cpKeyData = packageData(tTupleData, tTupleDesc, tableOid, eKeyUsage);

	if (cpKeyData == NULL)
	{
		elog(ERROR, "table lacks primary key");
		return -1;
	}

	planData[0] = PointerGetDatum(isKey);
	planData[1] = PointerGetDatum(cpKeyData);
	iRetValue = SPI_execp(pplan, planData, NULL, 1);

	if (cpKeyData != 0)
		SPI_pfree(cpKeyData);

	if (iRetValue != SPI_OK_INSERT)
	{
		elog(NOTICE, "error inserting row in pendingDelete");
		return -1;
	}

	debug_msg("dbmirror:storeKeyData insert successful");


	return 0;

}

/**
 * Packages the data in tTupleData into a string of the format
 * FieldName='value text'  where any quotes inside of value text
 * are escaped with a backslash and any backslashes in value text
 * are esacped by a second back slash.
 *
 * tTupleDesc should be a description of the tuple stored in
 * tTupleData.
 *
 * eFieldUsage specifies which fields to use.
 *	PRIMARY implies include only primary key fields.
 *	NONPRIMARY implies include only non-primary key fields.
 *	ALLKEYS implies include only primary and foreign key fields.
 *	ALL implies include all fields.
 */
char *
packageData(HeapTuple tTupleData, TupleDesc tTupleDesc, Oid tableOid,
			enum FieldUsage eKeyUsage)
{
	int			iNumCols;
	int2vector *tpPKeys = NULL;
	ArrayType  *tpFKeys = NULL;
	int			iColumnCounter;
	char	   *cpDataBlock;
	char	               *cpDataBlock_tmp;
	int			iDataBlockSize;
	int			iUsedDataBlock;
	int                     iBlockLen;

	iNumCols = tTupleDesc->natts;

	debug_msg2("dbmirror:packageData table oid = %i", tableOid);

	// Get PKs if required
	if (eKeyUsage != ALL)
	{
		tpPKeys = getPrimaryKey(tableOid);
		if (tpPKeys == NULL)
			return NULL;
	}

	if (tpPKeys != NULL)
		debug_msg("dbmirror:packageData have primary keys");

	// Get FKs if required
	if (eKeyUsage == ALLKEYS)
	{
		tpFKeys = getForeignKey(tableOid);
	}
	if (tpFKeys != NULL)
		debug_msg("dbmirror:packageData have foreign keys");

	cpDataBlock = SPI_palloc(BUFFER_SIZE);
	iDataBlockSize = BUFFER_SIZE;
	iUsedDataBlock = 0;			/* To account for the null */

	for (iColumnCounter = 1; iColumnCounter <= iNumCols; iColumnCounter++)
	{
		int			iIsPrimaryKey;
		int			iPrimaryKeyIndex;
		int			iIsForeignKey;
		int			iForeignKeyIndex;
		int			currentFKindex;
		char	   *cpUnFormatedPtr;
		char	   *cpFormatedPtr;

		char	   *cpFieldName;
		char	   *cpFieldData;

		if (eKeyUsage != ALL)
		{
			/* Determine if this is a primary key or not. */
			iIsPrimaryKey = 0;
			for (iPrimaryKeyIndex = 0;
				 iPrimaryKeyIndex < tpPKeys->dim1;
				 iPrimaryKeyIndex++)
			{
				if (tpPKeys->values[iPrimaryKeyIndex] == iColumnCounter)
				{
					iIsPrimaryKey = 1;
					break;
				}
			}
			/* Determine if this is a foreign key or not. */
			iIsForeignKey = 0;
			for (iForeignKeyIndex = 0;
				 tpFKeys != NULL && iForeignKeyIndex < ARR_DIMS(tpFKeys)[0];
				 iForeignKeyIndex++)
			{
				currentFKindex = ((short *) ARR_DATA_PTR(tpFKeys))[iForeignKeyIndex];
				debug_msg3("dbmirror:packageData analyzing FK: column %i / fk %i", iColumnCounter, currentFKindex);
				if (currentFKindex == iColumnCounter)
				{
					iIsForeignKey = 1;
					break;
				}
			}

			if ( (iIsPrimaryKey && (eKeyUsage == NONPRIMARY))
				|| (iIsForeignKey && (eKeyUsage == PRIMARY))
				|| (!iIsPrimaryKey && !iIsForeignKey && (eKeyUsage != NONPRIMARY)) )
			{
				/**
				 * Don't use.
				 */

				debug_msg2("dbmirror:packageData skipping column %i", iColumnCounter);

				continue;
			}
		}						/* KeyUsage!=ALL */

		if (tTupleDesc->attrs[iColumnCounter - 1]->attisdropped)
		{
			/**
			 * This column has been dropped.
			 * Do not mirror it.
			 */
			continue;
		}

		cpFieldName = DatumGetPointer(NameGetDatum

									  (&tTupleDesc->attrs
									   [iColumnCounter - 1]->attname));

		debug_msg2("dbmirror:packageData field name: %s", cpFieldName);

		while (iDataBlockSize - iUsedDataBlock <
			   strlen(cpFieldName) + 6)
		{
			cpDataBlock = SPI_repalloc(cpDataBlock,
									   iDataBlockSize +
									   BUFFER_SIZE);
			iDataBlockSize = iDataBlockSize + BUFFER_SIZE;
		}
		sprintf(cpDataBlock + iUsedDataBlock, "\"%s\"=", cpFieldName);
		iUsedDataBlock = iUsedDataBlock + strlen(cpFieldName) + 3;
		cpFieldData = SPI_getvalue(tTupleData, tTupleDesc,
								   iColumnCounter);

		cpUnFormatedPtr = cpFieldData;
		cpFormatedPtr = cpDataBlock + iUsedDataBlock;
		if (cpFieldData != NULL)
		{
			*cpFormatedPtr = '\'';
			iUsedDataBlock++;
			cpFormatedPtr++;
		}
		else
		{
			sprintf(cpFormatedPtr, " ");
			iUsedDataBlock++;
			cpFormatedPtr++;
			continue;

		}
		debug_msg2("dbmirror:packageData field data: \"%s\"",
				   cpFieldData);
		debug_msg("dbmirror:packageData starting format loop");

		while (*cpUnFormatedPtr != 0)
		{
			while (iDataBlockSize - iUsedDataBlock < 2)
			{
				cpDataBlock = SPI_repalloc(cpDataBlock,
										   iDataBlockSize
										   + BUFFER_SIZE);
				iDataBlockSize = iDataBlockSize + BUFFER_SIZE;
				cpFormatedPtr = cpDataBlock + iUsedDataBlock;
			}
			if (*cpUnFormatedPtr == '\\' || *cpUnFormatedPtr == '\'')
			{
				*cpFormatedPtr = *cpUnFormatedPtr;
				cpFormatedPtr++;
				iUsedDataBlock++;
			}
			*cpFormatedPtr = *cpUnFormatedPtr;
			cpFormatedPtr++;
			cpUnFormatedPtr++;
			iUsedDataBlock++;
		}

		SPI_pfree(cpFieldData);

		while (iDataBlockSize - iUsedDataBlock < 3)
		{
			cpDataBlock = SPI_repalloc(cpDataBlock,
									   iDataBlockSize +
									   BUFFER_SIZE);
			iDataBlockSize = iDataBlockSize + BUFFER_SIZE;
			cpFormatedPtr = cpDataBlock + iUsedDataBlock;
		}
		sprintf(cpFormatedPtr, "' ");
		iUsedDataBlock = iUsedDataBlock + 2;

		debug_msg2("dbmirror:packageData data block: \"%s\"",
				   cpDataBlock);

	}							/* for iColumnCounter  */
	if (tpPKeys != NULL)
		SPI_pfree(tpPKeys);
	if (tpFKeys != NULL)
		SPI_pfree(tpFKeys);

	debug_msg3("dbmirror:packageData returning DataBlockSize:%d iUsedDataBlock:%d",
			   iDataBlockSize,
			   iUsedDataBlock);

	memset(cpDataBlock + iUsedDataBlock, 0, iDataBlockSize - iUsedDataBlock);

        iBlockLen = strlen(cpDataBlock);
        cpDataBlock_tmp = SPI_palloc(VARHDRSZ+iBlockLen);
        memcpy((cpDataBlock_tmp+VARHDRSZ), cpDataBlock, iBlockLen);
        SET_VARSIZE(cpDataBlock_tmp, VARHDRSZ+iBlockLen);

        SPI_pfree(cpDataBlock);

	return cpDataBlock_tmp;
}


/*
 * Support for mirroring sequence objects.
 */

PG_FUNCTION_INFO_V1(setval_mirror);

Datum
setval_mirror(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		next = PG_GETARG_INT64(1);
	int64		result;

	result = DatumGetInt64(DirectFunctionCall2(setval_oid,
											   ObjectIdGetDatum(relid),
											   Int64GetDatum(next)));

	saveSequenceUpdate(relid, result, true);

	PG_RETURN_INT64(result);
}

PG_FUNCTION_INFO_V1(setval3_mirror);

Datum
setval3_mirror(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		next = PG_GETARG_INT64(1);
	bool		iscalled = PG_GETARG_BOOL(2);
	int64		result;

	result = DatumGetInt64(DirectFunctionCall3(setval3_oid,
											   ObjectIdGetDatum(relid),
											   Int64GetDatum(next),
											   BoolGetDatum(iscalled)));

	saveSequenceUpdate(relid, result, iscalled);

	PG_RETURN_INT64(result);
}

PG_FUNCTION_INFO_V1(nextval_mirror);

Datum
nextval_mirror(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;

	result = DatumGetInt64(DirectFunctionCall1(nextval_oid,
											   ObjectIdGetDatum(relid)));

	saveSequenceUpdate(relid, result, true);

	PG_RETURN_INT64(result);
}


static void
saveSequenceUpdate(Oid relid, int64 nextValue, bool iscalled)
{
	Oid			insertArgTypes[2] = {NAMEOID, INT4OID};
	Oid			insertDataArgTypes[1] = {NAMEOID};
	void	   *insertPlan;
	void	   *insertDataPlan;
	Datum		insertDatum[2];
	Datum		insertDataDatum[1];
	char		nextSequenceText[64];

	const char *insertQuery =
	"INSERT INTO dbmirror_Pending (TableName,Op,XID) VALUES" \
	"($1,'s',$2)";
	const char *insertDataQuery =
	"INSERT INTO dbmirror_PendingData(SeqId,IsKey,Data) VALUES " \
	"(currval('dbmirror_pending_seqid_seq'),'t',$1)";

	if (SPI_connect() < 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
			errmsg("dbmirror:savesequenceupdate could not connect to SPI")));

	insertPlan = SPI_prepare(insertQuery, 2, insertArgTypes);
	insertDataPlan = SPI_prepare(insertDataQuery, 1, insertDataArgTypes);

	if (insertPlan == NULL || insertDataPlan == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("dbmirror:savesequenceupdate error creating plan")));

	insertDatum[0] = PointerGetDatum(get_rel_name(relid));
	insertDatum[1] = Int32GetDatum(GetCurrentTransactionId());

	snprintf(nextSequenceText, sizeof(nextSequenceText),
			 INT64_FORMAT ",'%c'",
			 nextValue, iscalled ? 't' : 'f');

	/*
	 * note type cheat here: we prepare a C string and then claim it is a
	 * NAME, which the system will coerce to varchar for us.
	 */
	insertDataDatum[0] = PointerGetDatum(nextSequenceText);

	debug_msg2("dbmirror:savesequenceupdate: Setting value as %s",
			   nextSequenceText);

	debug_msg("dbmirror:About to execute insert query");

	if (SPI_execp(insertPlan, insertDatum, NULL, 1) != SPI_OK_INSERT)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("error inserting row in dbmirror_Pending")));

	if (SPI_execp(insertDataPlan, insertDataDatum, NULL, 1) != SPI_OK_INSERT)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("error inserting row in dbmirror_PendingData")));

	debug_msg("dbmirror:Insert query finished");
	SPI_pfree(insertPlan);
	SPI_pfree(insertDataPlan);

	SPI_finish();
}
