/*
 * src/bin/pgcopydb/summary.c
 *     Implementation of a CLI to copy a database between two Postgres instances
 */

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>

#include "parson.h"

#include "copydb.h"
#include "env_utils.h"
#include "log.h"
#include "pidfile.h"
#include "schema.h"
#include "signals.h"
#include "string_utils.h"
#include "summary.h"


static void prepareLineSeparator(char dashes[], int size);


/*
 * create_table_summary creates a summary file for the copy operation of a
 * given table. The summary file contains identification information and
 * duration information and can be used both as a lock file and as a resource
 * file to display what's happening.
 */
bool
write_table_summary(CopyTableSummary *summary, char *filename)
{
	char contents[BUFSIZE] = { 0 };

	sformat(contents, BUFSIZE,
			"%d\n%u\n%s\n%s\n%lld\n%lld\n%lld\n%s\n",
			summary->pid,
			summary->table->oid,
			summary->table->nspname,
			summary->table->relname,
			(long long) summary->startTime,
			(long long) summary->doneTime,
			(long long) summary->durationMs,
			summary->command);

	/* write the summary to the doneFile */
	if (!write_file(contents, strlen(contents), filename))
	{
		log_error("Failed to write table summary file \"%s\"", filename);
		return false;
	}

	return true;
}


/*
 * prepare_table_summary_as_json prepares the summary information as a JSON
 * object within the given JSON_Value.
 */
bool
prepare_table_summary_as_json(CopyTableSummary *summary,
							  JSON_Object *jsobj,
							  const char *key)
{
	JSON_Value *jsSummary = json_value_init_object();
	JSON_Object *jsSummaryObj = json_value_get_object(jsSummary);

	json_object_set_number(jsSummaryObj, "pid", (double) summary->pid);

	json_object_set_number(jsSummaryObj,
						   "start-time-epoch",
						   (double) summary->startTime);

	/* pretty print start time */
	time_t secs = summary->startTime;
	struct tm ts = { 0 };
	char startTimeStr[BUFSIZE] = { 0 };

	if (localtime_r(&secs, &ts) == NULL)
	{
		log_error("Failed to convert seconds %lld to local time: %m",
				  (long long) secs);
		return false;
	}

	strftime(startTimeStr, sizeof(startTimeStr), "%Y-%m-%d %H:%M:%S %Z", &ts);

	json_object_set_string(jsSummaryObj,
						   "start-time-string",
						   startTimeStr);

	json_object_set_string(jsSummaryObj, "command", summary->command);

	/* attach the JSON array to the main JSON object under the provided key */
	json_object_set_value(jsobj, key, jsSummary);

	return true;
}


/*
 * read_table_summary reads a summary on-disk and parses the content of the
 * file, and populates the given summary with what we read in the on-disk file.
 *
 * Couple notes:
 *
 *  1. the summary->table SourceTable pointer should point to allocated memory,
 *
 *  2. the instr_time fields can't be read from their on-disk representation
 *     and are set to zero instead.
 */
bool
read_table_summary(CopyTableSummary *summary, const char *filename)
{
	char *fileContents = NULL;
	long fileSize = 0L;

	if (!read_file(filename, &fileContents, &fileSize))
	{
		/* errors have already been logged */
		return false;
	}

	char *fileLines[BUFSIZE] = { 0 };
	int lineCount = splitLines(fileContents, fileLines, BUFSIZE);

	if (lineCount < COPY_TABLE_SUMMARY_LINES)
	{
		log_error("Failed to parse summary file \"%s\" which contains only "
				  "%d lines, at least %d lines are expected",
				  filename,
				  lineCount,
				  COPY_TABLE_SUMMARY_LINES);

		free(fileContents);

		return false;
	}

	if (!stringToInt(fileLines[0], &(summary->pid)))
	{
		/* errors have already been logged */
		return false;
	}

	/* better not point to NULL */
	SourceTable *table = summary->table;

	if (table == NULL)
	{
		log_error("BUG: read_table_summary summary->table is NULL");
		return false;
	}

	if (!stringToUInt32(fileLines[1], &(table->oid)))
	{
		/* errors have already been logged */
		return false;
	}

	strlcpy(table->nspname, fileLines[2], sizeof(table->nspname));
	strlcpy(table->relname, fileLines[3], sizeof(table->relname));

	if (!stringToUInt64(fileLines[4], &(summary->startTime)))
	{
		/* errors have already been logged */
		return false;
	}

	if (!stringToUInt64(fileLines[5], &(summary->doneTime)))
	{
		/* errors have already been logged */
		return false;
	}

	if (!stringToUInt64(fileLines[6], &(summary->durationMs)))
	{
		/* errors have already been logged */
		return false;
	}

	/* last summary line in the file is the SQL command */
	strlcpy(summary->command, fileLines[7], sizeof(summary->command));

	/* we can't provide instr_time readers */
	summary->startTimeInstr = (instr_time) {
		0
	};
	summary->durationInstr = (instr_time) {
		0
	};

	return true;
}


/*
 * open_table_summary initializes the time elements of a table summary and
 * writes the summary in the given filename. Typically, the lockFile.
 */
bool
open_table_summary(CopyTableSummary *summary, char *filename)
{
	summary->startTime = time(NULL);
	summary->doneTime = 0;
	summary->durationMs = 0;
	summary->startTimeInstr = (instr_time) {
		0
	};
	summary->durationInstr = (instr_time) {
		0
	};

	INSTR_TIME_SET_CURRENT(summary->startTimeInstr);

	return write_table_summary(summary, filename);
}


/*
 * finish_table_summary sets the duration of the summary fields and writes the
 * summary in the given filename. Typically, the doneFile.
 */
bool
finish_table_summary(CopyTableSummary *summary, char *filename)
{
	summary->doneTime = time(NULL);

	INSTR_TIME_SET_CURRENT(summary->durationInstr);
	INSTR_TIME_SUBTRACT(summary->durationInstr, summary->startTimeInstr);

	summary->durationMs = INSTR_TIME_GET_MILLISEC(summary->durationInstr);

	return write_table_summary(summary, filename);
}


/*
 * create_table_index_file creates a file with one line per index attached to a
 * table. Each line contains only the index oid, from which we can find the
 * index doneFile.
 */
bool
create_table_index_file(SourceTable *table, char *filename)
{
	PQExpBuffer content = createPQExpBuffer();

	if (content == NULL)
	{
		log_fatal("Failed to allocate memory to create the "
				  " index list file \"%s\"", filename);
		return false;
	}

	SourceIndexList *indexListEntry = table->firstIndex;

	for (; indexListEntry != NULL; indexListEntry = indexListEntry->next)
	{
		SourceIndex *index = indexListEntry->index;

		appendPQExpBuffer(content, "%d\n", index->indexOid);
		appendPQExpBuffer(content, "%d\n", index->constraintOid);
	}

	/* memory allocation could have failed while building string */
	if (PQExpBufferBroken(content))
	{
		log_error("Failed to create file \"%s\": out of memory", filename);
		destroyPQExpBuffer(content);
		return false;
	}

	if (!write_file(content->data, content->len, filename))
	{
		log_error("Failed to write file \"%s\"", filename);
		destroyPQExpBuffer(content);
		return false;
	}

	return true;
}


/*
 * read_table_index_file reads an index list file and populates an array of
 * indexes with only the indexOid information. The actual array memory
 * allocation is done in the function.
 */
bool
read_table_index_file(SourceIndexArray *indexArray, char *filename)
{
	char *fileContents = NULL;
	long fileSize = 0L;

	if (!file_exists(filename))
	{
		indexArray->count = 0;
		indexArray->array = NULL;
		return true;
	}

	if (!read_file(filename, &fileContents, &fileSize))
	{
		/* errors have already been logged */
		return false;
	}

	char *fileLines[BUFSIZE] = { 0 };
	int lineCount = splitLines(fileContents, fileLines, BUFSIZE);

	/*
	 * We expect to have alternate lines with first indexOid and then
	 * constraintOid (which could be zero). No comments, etc.
	 */
	indexArray->count = lineCount / 2;
	indexArray->array = (SourceIndex *) calloc(lineCount, sizeof(SourceIndex));

	int pos = 0;

	for (int i = 0; i < lineCount; i++)
	{
		SourceIndex *index = &(indexArray->array[pos]);

		uint32_t *target =
			i % 2 == 0 ? &(index->indexOid) : &(index->constraintOid);

		if (!stringToUInt32(fileLines[i], target))
		{
			log_error("Failed to read the index oid \"%s\" "
					  "in file \"%s\" at line %d",
					  fileLines[i],
					  filename,
					  i);
			return false;
		}

		/* one index entry (indexOid, constraintOid) spans two lines */
		if (i % 2 == 1)
		{
			pos++;
		}
	}

	return true;
}


/*
 * write_index_summary writes the current Index Summary to given filename. The
 * constraint bool allows to write the constraint definition instead of the
 * index definition.
 */
bool
write_index_summary(CopyIndexSummary *summary, char *filename, bool constraint)
{
	char contents[BUFSIZE] = { 0 };

	uint32_t oid =
		constraint
		? summary->index->constraintOid
		: summary->index->indexOid;

	char *name =
		constraint
		? summary->index->constraintName
		: summary->index->indexRelname;

	sformat(contents, BUFSIZE,
			"%d\n%u\n%s\n%s\n%lld\n%lld\n%lld\n%s\n",
			summary->pid,
			oid,
			summary->index->indexNamespace,
			name,
			(long long) summary->startTime,
			(long long) summary->doneTime,
			(long long) summary->durationMs,
			summary->command);

	/* write the summary to the doneFile */
	return write_file(contents, strlen(contents), filename);
}


/*
 * read_index_summary reads back in-memory a summary from disk.
 */
bool
read_index_summary(CopyIndexSummary *summary, const char *filename)
{
	char *fileContents = NULL;
	long fileSize = 0L;

	if (!read_file(filename, &fileContents, &fileSize))
	{
		/* errors have already been logged */
		return false;
	}

	char *fileLines[BUFSIZE] = { 0 };
	int lineCount = splitLines(fileContents, fileLines, BUFSIZE);

	if (lineCount < COPY_TABLE_SUMMARY_LINES)
	{
		log_error("Failed to parse summary file \"%s\" which contains only "
				  "%d lines, at least %d lines are expected",
				  filename,
				  lineCount,
				  COPY_TABLE_SUMMARY_LINES);

		free(fileContents);

		return false;
	}

	if (!stringToInt(fileLines[0], &(summary->pid)))
	{
		/* errors have already been logged */
		return false;
	}

	/* better not point to NULL */
	SourceIndex *index = summary->index;

	if (index == NULL)
	{
		log_error("BUG: read_index_summary summary->index is NULL");
		return false;
	}

	if (!stringToUInt32(fileLines[1], &(index->indexOid)))
	{
		/* errors have already been logged */
		return false;
	}

	strlcpy(index->indexNamespace, fileLines[2], sizeof(index->indexNamespace));
	strlcpy(index->indexRelname, fileLines[3], sizeof(index->indexRelname));

	if (!stringToUInt64(fileLines[4], &(summary->startTime)))
	{
		/* errors have already been logged */
		return false;
	}

	if (!stringToUInt64(fileLines[5], &(summary->doneTime)))
	{
		/* errors have already been logged */
		return false;
	}

	if (!stringToUInt64(fileLines[6], &(summary->durationMs)))
	{
		/* errors have already been logged */
		return false;
	}

	/* last summary line in the file is the SQL command */
	strlcpy(summary->command, fileLines[7], sizeof(summary->command));

	/* we can't provide instr_time readers */
	summary->startTimeInstr = (instr_time) {
		0
	};
	summary->durationInstr = (instr_time) {
		0
	};

	return true;
}


/*
 * prepare_index_summary_as_json prepares the summary information as a JSON
 * object within the given JSON_Value.
 */
bool
prepare_index_summary_as_json(CopyIndexSummary *summary,
							  JSON_Object *jsobj,
							  const char *key)
{
	JSON_Value *jsSummary = json_value_init_object();
	JSON_Object *jsSummaryObj = json_value_get_object(jsSummary);

	json_object_set_number(jsSummaryObj, "pid", (double) summary->pid);

	json_object_set_number(jsSummaryObj,
						   "start-time-epoch",
						   (double) summary->startTime);

	/* pretty print start time */
	time_t secs = summary->startTime;
	struct tm ts = { 0 };
	char startTimeStr[BUFSIZE] = { 0 };

	if (localtime_r(&secs, &ts) == NULL)
	{
		log_error("Failed to convert seconds %lld to local time: %m",
				  (long long) secs);
		return false;
	}

	strftime(startTimeStr, sizeof(startTimeStr), "%Y-%m-%d %H:%M:%S %Z", &ts);

	json_object_set_string(jsSummaryObj,
						   "start-time-string",
						   startTimeStr);

	/* attach the JSON array to the main JSON object under the provided key */
	json_object_set_value(jsobj, key, jsSummary);

	return true;
}


/*
 * open_index_summary initializes the time elements of an index summary and
 * writes the summary in the given filename. Typically, the lockFile.
 */
bool
open_index_summary(CopyIndexSummary *summary, char *filename, bool constraint)
{
	summary->startTime = time(NULL);
	summary->doneTime = 0;
	summary->durationMs = 0;
	summary->startTimeInstr = (instr_time) {
		0
	};
	summary->durationInstr = (instr_time) {
		0
	};

	INSTR_TIME_SET_CURRENT(summary->startTimeInstr);

	return write_index_summary(summary, filename, constraint);
}


/*
 * finish_index_summary sets the duration of the summary fields and writes the
 * summary in the given filename. Typically, the doneFile.
 */
bool
finish_index_summary(CopyIndexSummary *summary, char *filename, bool constraint)
{
	summary->doneTime = time(NULL);

	INSTR_TIME_SET_CURRENT(summary->durationInstr);
	INSTR_TIME_SUBTRACT(summary->durationInstr, summary->startTimeInstr);

	summary->durationMs = INSTR_TIME_GET_MILLISEC(summary->durationInstr);

	return write_index_summary(summary, filename, constraint);
}


/*
 * write_blobs_summary writes the given pre-filled summary to disk.
 */
bool
write_blobs_summary(CopyBlobsSummary *summary, char *filename)
{
	char contents[BUFSIZE] = { 0 };

	sformat(contents, sizeof(contents), "%d\n%lld\n%lld\n",
			summary->pid,
			(long long) summary->count,
			(long long) summary->durationMs);

	if (!write_file(contents, strlen(contents), filename))
	{
		log_warn("Failed to write the tracking file \%s\"", filename);
		return false;
	}

	return true;
}


/*
 * read_blobs_summary reads a blobs process summary file from disk.
 */
bool
read_blobs_summary(CopyBlobsSummary *summary, char *filename)
{
	char *fileContents = NULL;
	long fileSize = 0L;

	if (!read_file(filename, &fileContents, &fileSize))
	{
		/* errors have already been logged */
		return false;
	}

	char *fileLines[BUFSIZE] = { 0 };
	int lineCount = splitLines(fileContents, fileLines, BUFSIZE);

	if (lineCount < COPY_BLOBS_SUMMARY_LINES)
	{
		log_error("Failed to parse summary file \"%s\" which contains only "
				  "%d lines, at least %d lines are expected",
				  filename,
				  lineCount,
				  COPY_BLOBS_SUMMARY_LINES);

		free(fileContents);

		return false;
	}

	if (!stringToInt(fileLines[0], &(summary->pid)))
	{
		/* errors have already been logged */
		return false;
	}

	if (!stringToUInt32(fileLines[1], &(summary->count)))
	{
		/* errors have already been logged */
		return false;
	}

	if (!stringToUInt64(fileLines[2], &(summary->durationMs)))
	{
		/* errors have already been logged */
		return false;
	}

	return true;
}


/*
 * summary_set_current_time sets the current timing to the appropriate
 * TopLevelTimings entry given the step we're at.
 */
void
summary_set_current_time(TopLevelTimings *timings, TimingStep step)
{
	switch (step)
	{
		case TIMING_STEP_START:
		{
			INSTR_TIME_SET_CURRENT(timings->startTime);
			break;
		}

		case TIMING_STEP_BEFORE_SCHEMA_FETCH:
		{
			INSTR_TIME_SET_CURRENT(timings->beforeSchemaFetch);
			break;
		}

		case TIMING_STEP_BEFORE_SCHEMA_DUMP:
		{
			INSTR_TIME_SET_CURRENT(timings->beforeSchemaDump);
			break;
		}

		case TIMING_STEP_BEFORE_PREPARE_SCHEMA:
		{
			INSTR_TIME_SET_CURRENT(timings->beforePrepareSchema);
			break;
		}

		case TIMING_STEP_AFTER_PREPARE_SCHEMA:
		{
			INSTR_TIME_SET_CURRENT(timings->afterPrepareSchema);
			break;
		}

		case TIMING_STEP_BEFORE_FINALIZE_SCHEMA:
		{
			INSTR_TIME_SET_CURRENT(timings->beforeFinalizeSchema);
			break;
		}

		case TIMING_STEP_AFTER_FINALIZE_SCHEMA:
		{
			INSTR_TIME_SET_CURRENT(timings->afterFinalizeSchema);
			break;
		}

		case TIMING_STEP_END:
		{
			INSTR_TIME_SET_CURRENT(timings->endTime);
			break;
		}
	}
}


/* avoid non-initialized durations or clock oddities */
#define INSTR_TIME_MS(x) \
	(INSTR_TIME_GET_MILLISEC(x) > 0 ? INSTR_TIME_GET_MILLISEC(x) : 0)

/*
 * summary_prepare_toplevel_durations prepares the top-level durations in a
 * form that's suitable for printing on-screen.
 */
void
summary_prepare_toplevel_durations(Summary *summary)
{
	TopLevelTimings *timings = &(summary->timings);

	instr_time duration;
	uint64_t durationMs;

	/* compute schema dump duration, part of schemaDurationMs */
	duration = timings->beforeSchemaFetch;
	INSTR_TIME_SUBTRACT(duration, timings->beforeSchemaDump);
	durationMs = INSTR_TIME_MS(duration);

	IntervalToString(durationMs, timings->dumpSchemaMs, INTSTRING_MAX_DIGITS);

	timings->schemaDurationMs = durationMs;

	/* compute schema fetch duration, part of schemaDurationMs */
	duration = timings->beforePrepareSchema;
	INSTR_TIME_SUBTRACT(duration, timings->beforeSchemaFetch);
	durationMs = INSTR_TIME_MS(duration);

	IntervalToString(durationMs, timings->fetchSchemaMs, INTSTRING_MAX_DIGITS);

	timings->schemaDurationMs += durationMs;

	/* compute prepare schema duration, part of schemaDurationMs */
	duration = timings->afterPrepareSchema;
	INSTR_TIME_SUBTRACT(duration, timings->beforePrepareSchema);
	durationMs = INSTR_TIME_MS(duration);

	IntervalToString(durationMs, timings->prepareSchemaMs, INTSTRING_MAX_DIGITS);

	timings->schemaDurationMs += durationMs;

	/* compute data + index duration, between prepare schema and finalize */
	duration = timings->beforeFinalizeSchema;
	INSTR_TIME_SUBTRACT(duration, timings->afterPrepareSchema);
	durationMs = INSTR_TIME_MS(duration);

	IntervalToString(durationMs, timings->dataAndIndexMs, INTSTRING_MAX_DIGITS);

	timings->dataAndIndexesDurationMs = durationMs;

	/* compute finalize schema duration, part of schemaDurationMs */
	duration = timings->afterFinalizeSchema;
	INSTR_TIME_SUBTRACT(duration, timings->beforeFinalizeSchema);
	durationMs = INSTR_TIME_MS(duration);

	IntervalToString(durationMs, timings->finalizeSchemaMs, INTSTRING_MAX_DIGITS);

	timings->schemaDurationMs += durationMs;

	/* compute total duration, wall clock elapsed time */
	duration = timings->endTime;
	INSTR_TIME_SUBTRACT(duration, timings->startTime);
	durationMs = INSTR_TIME_MS(duration);

	IntervalToString(durationMs, timings->totalMs, INTSTRING_MAX_DIGITS);

	timings->totalDurationMs = durationMs;

	/* prepare the pretty printed string for the cumulative parallel part */
	IntervalToString(timings->tableDurationMs,
					 timings->totalTableMs,
					 INTSTRING_MAX_DIGITS);

	IntervalToString(timings->indexDurationMs,
					 timings->totalIndexMs,
					 INTSTRING_MAX_DIGITS);
}


/*
 * print_toplevel_summary prints a summary of the top-level timings.
 */
void
print_toplevel_summary(Summary *summary, int tableJobs, int indexJobs)
{
	char *d10s = "----------";
	char *d12s = "------------";
	char *d50s = "--------------------------------------------------";

	fformat(stdout, "\n");

	fformat(stdout, " %50s   %10s  %10s  %12s\n",
			"Step", "Connection", "Duration", "Concurrency");

	fformat(stdout, " %50s   %10s  %10s  %12s\n", d50s, d10s, d10s, d12s);

	fformat(stdout, " %50s   %10s  %10s  %12d\n", "Dump Schema", "source",
			summary->timings.dumpSchemaMs, 1);

	fformat(stdout, " %50s   %10s  %10s  %12d\n",
			"Catalog Queries (table ordering, filtering, etc)",
			"source",
			summary->timings.fetchSchemaMs,
			1);

	fformat(stdout, " %50s   %10s  %10s  %12d\n", "Prepare Schema", "target",
			summary->timings.prepareSchemaMs, 1);

	char concurrency[BUFSIZE] = { 0 };
	sformat(concurrency, sizeof(concurrency), "%d + %d",
			tableJobs,
			tableJobs + indexJobs);

	fformat(stdout, " %50s   %10s  %10s  %12s\n",
			"COPY, INDEX, CONSTRAINTS, VACUUM (wall clock)", "both",
			summary->timings.dataAndIndexMs,
			concurrency);

	fformat(stdout, " %50s   %10s  %10s  %12d\n",
			"COPY (cumulative)", "both",
			summary->timings.totalTableMs,
			tableJobs);

	fformat(stdout, " %50s   %10s  %10s  %12d\n",
			"Large Objects (cumulative)", "both",
			summary->timings.blobsMs,
			1);

	fformat(stdout, " %50s   %10s  %10s  %12d\n",
			"CREATE INDEX, CONSTRAINTS (cumulative)", "target",
			summary->timings.totalIndexMs,
			indexJobs);

	fformat(stdout, " %50s   %10s  %10s  %12d\n", "Finalize Schema", "target",
			summary->timings.finalizeSchemaMs, 1);

	fformat(stdout, " %50s   %10s  %10s  %12s\n", d50s, d10s, d10s, d12s);

	fformat(stdout, " %50s   %10s  %10s  %12s\n",
			"Total Wall Clock Duration", "both",
			summary->timings.totalMs,
			concurrency);

	fformat(stdout, " %50s   %10s  %10s  %12s\n", d50s, d10s, d10s, d12s);

	fformat(stdout, "\n");
}


/*
 * print_summary_table loops over a fully prepared summary table and prints
 * each element. It also prints the headers.
 */
void
print_summary_table(SummaryTable *summary)
{
	SummaryTableHeaders *headers = &(summary->headers);

	fformat(stdout, "\n");

	fformat(stdout, "%*s | %*s | %*s | %*s | %*s | %*s\n",
			headers->maxOidSize, "OID",
			headers->maxNspnameSize, "Schema",
			headers->maxRelnameSize, "Name",
			headers->maxTableMsSize, "copy duration",
			headers->maxIndexCountSize, "indexes",
			headers->maxIndexMsSize, "create index duration");

	fformat(stdout, "%s-+-%s-+-%s-+-%s-+-%s-+-%s\n",
			headers->oidSeparator,
			headers->nspnameSeparator,
			headers->relnameSeparator,
			headers->tableMsSeparator,
			headers->indexCountSeparator,
			headers->indexMsSeparator);

	for (int i = 0; i < summary->count; i++)
	{
		SummaryTableEntry *entry = &(summary->array[i]);

		fformat(stdout, "%*s | %*s | %*s | %*s | %*s | %*s\n",
				headers->maxOidSize, entry->oid,
				headers->maxNspnameSize, entry->nspname,
				headers->maxRelnameSize, entry->relname,
				headers->maxTableMsSize, entry->tableMs,
				headers->maxIndexCountSize, entry->indexCount,
				headers->maxIndexMsSize, entry->indexMs);
	}

	fformat(stdout, "\n");
}


/*
 * prepare_summary_table_headers computes the actual max length of all the
 * columns that we are going to display, and fills in the dashed separators
 * too.
 */
void
prepare_summary_table_headers(SummaryTable *summary)
{
	SummaryTableHeaders *headers = &(summary->headers);

	/* assign static maximums from the lenghts of the column headers */
	headers->maxOidSize = 3;        /* "oid" */
	headers->maxNspnameSize = 6;    /* "schema" */
	headers->maxRelnameSize = 4;    /* "name" */
	headers->maxTableMsSize = 13;   /* "copy duration" */
	headers->maxIndexCountSize = 7; /* "indexes" */
	headers->maxIndexMsSize = 21;   /* "create index duration" */

	/* now adjust to the actual table's content */
	for (int i = 0; i < summary->count; i++)
	{
		int len = 0;
		SummaryTableEntry *entry = &(summary->array[i]);

		len = strlen(entry->oid);

		if (headers->maxOidSize < len)
		{
			headers->maxOidSize = len;
		}

		len = strlen(entry->nspname);

		if (headers->maxNspnameSize < len)
		{
			headers->maxNspnameSize = len;
		}

		len = strlen(entry->relname);

		if (headers->maxRelnameSize < len)
		{
			headers->maxRelnameSize = len;
		}

		len = strlen(entry->tableMs);

		if (headers->maxTableMsSize < len)
		{
			headers->maxTableMsSize = len;
		}

		len = strlen(entry->indexCount);

		if (headers->maxIndexCountSize < len)
		{
			headers->maxIndexCountSize = len;
		}

		len = strlen(entry->indexMs);

		if (headers->maxIndexMsSize < len)
		{
			headers->maxIndexMsSize = len;
		}
	}

	/* now prepare the header line with dashes */
	prepareLineSeparator(headers->oidSeparator, headers->maxOidSize);
	prepareLineSeparator(headers->nspnameSeparator, headers->maxNspnameSize);
	prepareLineSeparator(headers->relnameSeparator, headers->maxRelnameSize);
	prepareLineSeparator(headers->tableMsSeparator, headers->maxTableMsSize);
	prepareLineSeparator(headers->indexCountSeparator, headers->maxIndexCountSize);
	prepareLineSeparator(headers->indexMsSeparator, headers->maxIndexMsSize);
}


/*
 * prepareLineSeparator fills in the pre-allocated given string with the
 * expected amount of dashes to use as a separator line in our tabular output.
 */
static void
prepareLineSeparator(char dashes[], int size)
{
	for (int i = 0; i <= size; i++)
	{
		if (i < size)
		{
			dashes[i] = '-';
		}
		else
		{
			dashes[i] = '\0';
			break;
		}
	}
}


/*
 * print_summary prints a summary of the pgcopydb operations on stdout.
 *
 * The summary contains a line per table that has been copied and then the
 * count of indexes created for each table, and then the sum of the timing of
 * creating those indexes.
 */
bool
print_summary(Summary *summary, CopyDataSpec *specs)
{
	SummaryTable *summaryTable = &(summary->table);

	/* first, we have to scan the available data from memory and files */
	if (!prepare_summary_table(summary, specs))
	{
		log_error("Failed to prepare the summary table");
		return false;
	}

	/* then we can prepare the headers and print the table */
	if (specs->section == DATA_SECTION_TABLE_DATA ||
		specs->section == DATA_SECTION_ALL)
	{
		(void) prepare_summary_table_headers(summaryTable);
		(void) print_summary_table(summaryTable);
	}

	/* and then finally prepare the top-level counters and print them */
	(void) summary_prepare_toplevel_durations(summary);
	(void) print_toplevel_summary(summary, specs->tableJobs, specs->indexJobs);

	return true;
}


/*
 * prepare_summary_table prepares the summary table array with the durations
 * read from disk in the doneFile for each oid that has been processed.
 */
bool
prepare_summary_table(Summary *summary, CopyDataSpec *specs)
{
	TopLevelTimings *timings = &(summary->timings);
	SummaryTable *summaryTable = &(summary->table);
	CopyTableDataSpecsArray *tableSpecsArray = &(specs->tableSpecsArray);

	int count = tableSpecsArray->count;

	summaryTable->count = count;
	summaryTable->array =
		(SummaryTableEntry *) malloc(count * sizeof(SummaryTableEntry));

	if (summaryTable->array == NULL)
	{
		log_error(ALLOCATION_FAILED_ERROR);
		return false;
	}

	for (int tableIndex = 0; tableIndex < tableSpecsArray->count; tableIndex++)
	{
		CopyTableDataSpec *tableSpecs = &(tableSpecsArray->array[tableIndex]);
		SourceTable *table = tableSpecs->sourceTable;

		SummaryTableEntry *entry = &(summaryTable->array[tableIndex]);

		/* prepare some of the information we already have */
		IntString oidString = intToString(table->oid);

		strlcpy(entry->oid, oidString.strValue, sizeof(entry->oid));
		strlcpy(entry->nspname, table->nspname, sizeof(entry->nspname));
		strlcpy(entry->relname, table->relname, sizeof(entry->relname));

		/* the specs doesn't contain timing information */
		CopyTableSummary tableSummary = { .table = table };

		if (!read_table_summary(&tableSummary, tableSpecs->tablePaths.doneFile))
		{
			/* errors have already been logged */
			return false;
		}

		timings->tableDurationMs += tableSummary.durationMs;

		(void) IntervalToString(tableSummary.durationMs,
								entry->tableMs,
								sizeof(entry->tableMs));

		/* read the index oid list from the table oid */
		uint64_t indexingDurationMs = 0;

		SourceIndexArray indexArray = { 0 };

		/*
		 * When the table COPY processing was split into several processes,
		 * ensure we only read the index list once: only one of those COPY
		 * processes started the indexing.
		 */
		if (tableSpecs->part.partNumber == 0)
		{
			if (!read_table_index_file(&indexArray,
									   tableSpecs->tablePaths.idxListFile))
			{
				/* errors have already been logged */
				return false;
			}

			/* for reach index, read the index summary */
			for (int i = 0; i < indexArray.count; i++)
			{
				SourceIndex *index = &(indexArray.array[i]);

				CopyFilePaths *cfPaths = &(specs->cfPaths);
				IndexFilePaths indexPaths = { 0 };

				if (!copydb_init_index_paths(cfPaths, index, &indexPaths))
				{
					/* errors have already been logged */
					return false;
				}

				/* when a table has no indexes, the file doesn't exists */
				if (file_exists(indexPaths.doneFile))
				{
					CopyIndexSummary indexSummary = { .index = index };

					if (!read_index_summary(&indexSummary, indexPaths.doneFile))
					{
						/* errors have already been logged */
						return false;
					}

					/* accumulate total duration of creating all the indexes */
					timings->indexDurationMs += indexSummary.durationMs;
					indexingDurationMs += indexSummary.durationMs;
				}

				if (file_exists(indexPaths.constraintDoneFile))
				{
					CopyIndexSummary indexSummary = { .index = index };

					if (!read_index_summary(&indexSummary,
											indexPaths.constraintDoneFile))
					{
						/* errors have already been logged */
						return false;
					}

					/* accumulate total duration of creating all the indexes */
					timings->indexDurationMs += indexSummary.durationMs;
					indexingDurationMs += indexSummary.durationMs;
				}
			}
		}

		IntString indexCountString = intToString(indexArray.count);

		strlcpy(entry->indexCount,
				indexCountString.strValue,
				sizeof(entry->indexCount));

		(void) IntervalToString(indexingDurationMs,
								entry->indexMs,
								sizeof(entry->indexMs));
	}

	/*
	 * Also read the blobs summary file.
	 */
	if (file_exists(specs->cfPaths.done.blobs))
	{
		CopyBlobsSummary blobsSummary = { 0 };

		if (!read_blobs_summary(&blobsSummary, specs->cfPaths.done.blobs))
		{
			/* errors have already been logged */
			return false;
		}

		timings->blobDurationMs = blobsSummary.durationMs;

		(void) IntervalToString(blobsSummary.durationMs,
								timings->blobsMs,
								sizeof(timings->blobsMs));
	}

	return true;
}
