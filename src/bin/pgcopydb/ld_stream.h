/*
 * src/bin/pgcopydb/ld_stream.h
 *	 SQL queries to discover the source database stream
 */

#ifndef LD_STREAM_H
#define LD_STREAM_H

#include <stdbool.h>

#include "parson.h"

#include "copydb.h"
#include "queue_utils.h"
#include "pgsql.h"

#define OUTPUT_BEGIN "BEGIN; -- "
#define OUTPUT_COMMIT "COMMIT; -- "
#define OUTPUT_SWITCHWAL "-- SWITCH WAL "
#define OUTPUT_KEEPALIVE "-- KEEPALIVE "

typedef enum
{
	STREAM_ACTION_UNKNOWN = 0,
	STREAM_ACTION_BEGIN = 'B',
	STREAM_ACTION_COMMIT = 'C',
	STREAM_ACTION_INSERT = 'I',
	STREAM_ACTION_UPDATE = 'U',
	STREAM_ACTION_DELETE = 'D',
	STREAM_ACTION_TRUNCATE = 'T',
	STREAM_ACTION_MESSAGE = 'M',
	STREAM_ACTION_SWITCH = 'X',
	STREAM_ACTION_KEEPALIVE = 'K'
} StreamAction;

typedef struct StreamCounters
{
	uint64_t total;
	uint64_t begin;
	uint64_t commit;
	uint64_t insert;
	uint64_t update;
	uint64_t delete;
	uint64_t truncate;
} StreamCounters;


#define PG_MAX_TIMESTAMP 36     /* "2022-06-27 14:42:21.795714+00" */

typedef struct LogicalMessageMetadata
{
	StreamAction action;
	uint32_t xid;
	uint64_t lsn;
	char timestamp[PG_MAX_TIMESTAMP];
	bool filterOut;
} LogicalMessageMetadata;


/*
 * The detailed behavior of the LogicalStreamClient is implemented in the
 * callback functions writeFunction, flushFunction, and closeFunction.
 *
 */
typedef enum
{
	STREAM_MODE_UNKNOW = 0,
	STREAM_MODE_RECEIVE,        /* pgcopydb receive */
	STREAM_MODE_PREFETCH,       /* pgcopydb fetch */
	STREAM_MODE_APPLY           /* pgcopydb replay */
} LogicalStreamMode;


typedef struct StreamContext
{
	CDCPaths paths;
	LogicalStreamMode mode;

	char source_pguri[MAXCONNINFO];

	uint64_t startpos;
	uint64_t endpos;
	bool apply;

	char *jsonBuffer;           /* malloc'ed area */
	LogicalMessageMetadata metadata;

	Queue transformQueue;
	uint32_t WalSegSz;
	uint32_t timeline;

	uint64_t firstLSN;
	char walFileName[MAXPGPATH];
	char sqlFileName[MAXPGPATH];
	FILE *jsonFile;

	pid_t subprocess;

	StreamCounters counters;
} StreamContext;


typedef struct StreamApplyContext
{
	CDCPaths paths;

	PGSQL pgsql;
	char source_pguri[MAXCONNINFO];
	char target_pguri[MAXCONNINFO];
	char origin[BUFSIZE];

	IdentifySystem system;      /* information about source database */
	uint32_t WalSegSz;          /* information about source database */

	uint64_t previousLSN;       /* register COMMIT LSN progress */

	bool apply;                 /* from the pgcopydb sentinel */
	uint64_t startpos;          /* from the pgcopydb sentinel */
	uint64_t endpos;            /* finish applying when endpos is reached */

	bool reachedEndPos;

	char wal[MAXPGPATH];
	char sqlFileName[MAXPGPATH];
} StreamApplyContext;


/* data types to support here are limited to what JSON/wal2json offers */
typedef struct LogicalMessageValue
{
	int oid;                    /* BOOLOID, INT8OID, FLOAT8OID, TEXTOID */
	bool isNull;
	bool isQuoted;

	union value
	{
		bool boolean;
		uint64_t int8;
		double float8;
		char *str;              /* malloc'ed area (strdup) */
	} val;
} LogicalMessageValue;

typedef struct LogicalMessageValues
{
	int cols;
	LogicalMessageValue *array; /* malloc'ed area */
} LogicalMessageValues;

typedef struct LogicalMessageValuesArray
{
	int count;
	LogicalMessageValues *array; /* malloc'ed area */
} LogicalMessageValuesArray;

typedef struct LogicalMessageTuple
{
	int cols;
	char **columns;                  /* malloc'ed area */
	LogicalMessageValuesArray values;
} LogicalMessageTuple;

typedef struct LogicalMessageTupleArray
{
	int count;
	LogicalMessageTuple *array; /* malloc'ed area */
} LogicalMessageTupleArray;

typedef struct LogicalMessageInsert
{
	char nspname[NAMEDATALEN];
	char relname[NAMEDATALEN];
	LogicalMessageTupleArray new;   /* {"columns": ...} */
} LogicalMessageInsert;

typedef struct LogicalMessageUpdate
{
	char nspname[NAMEDATALEN];
	char relname[NAMEDATALEN];
	LogicalMessageTupleArray old;   /* {"identity": ...} */
	LogicalMessageTupleArray new;   /* {"columns": ...} */
} LogicalMessageUpdate;

typedef struct LogicalMessageDelete
{
	char nspname[NAMEDATALEN];
	char relname[NAMEDATALEN];
	LogicalMessageTupleArray old;   /* {"identity": ...} */
} LogicalMessageDelete;

typedef struct LogicalMessageTruncate
{
	char nspname[NAMEDATALEN];
	char relname[NAMEDATALEN];
} LogicalMessageTruncate;

typedef struct LogicalMessageSwitchWAL
{
	uint64_t lsn;
} LogicalMessageSwitchWAL;

typedef struct LogicalMessageKeepalive
{
	uint64_t lsn;
	char timestamp[PG_MAX_TIMESTAMP];
} LogicalMessageKeepalive;


/*
 * The JSON-lines logical decoding stream is then parsed into transactions that
 * contains a series of insert/update/delete/truncate commands.
 */
typedef struct LogicalTransactionStatement
{
	StreamAction action;

	union stmt
	{
		LogicalMessageInsert insert;
		LogicalMessageUpdate update;
		LogicalMessageDelete delete;
		LogicalMessageTruncate truncate;
		LogicalMessageSwitchWAL switchwal;
		LogicalMessageKeepalive keepalive;
	} stmt;

	struct LogicalTransactionStatement *prev; /* double linked-list */
	struct LogicalTransactionStatement *next; /* double linked-list */
} LogicalTransactionStatement;


typedef struct LogicalTransaction
{
	uint32_t xid;
	uint64_t beginLSN;
	uint64_t commitLSN;
	char timestamp[PG_MAX_TIMESTAMP];

	uint32_t count;                     /* number of statements */
	LogicalTransactionStatement *first;
	LogicalTransactionStatement *last;
} LogicalTransaction;

typedef struct LogicalTransactionArray
{
	int count;
	LogicalTransaction *array; /* malloc'ed area */
} LogicalTransactionArray;


typedef struct StreamSpecs
{
	CDCPaths paths;

	char source_pguri[MAXCONNINFO];
	char logrep_pguri[MAXCONNINFO];
	char target_pguri[MAXCONNINFO];

	StreamOutputPlugin plugin;
	KeyVal pluginOptions;

	char slotName[NAMEDATALEN];
	char origin[NAMEDATALEN];

	uint64_t startpos;
	uint64_t endpos;

	LogicalStreamMode mode;

	bool restart;
	bool resume;
} StreamSpecs;

typedef struct StreamContent
{
	char filename[MAXPGPATH];
	int count;
	char *buffer;
	char **lines;                     /* malloc'ed area */
	LogicalMessageMetadata *messages; /* malloc'ed area */
} StreamContent;


bool stream_init_specs(StreamSpecs *specs,
					   CDCPaths *paths,
					   char *source_pguri,
					   char *target_pguri,
					   char *plugin,
					   char *slotName,
					   char *origin,
					   uint64_t endpos,
					   LogicalStreamMode mode);

bool stream_init_context(StreamContext *privateContext, StreamSpecs *specs);
bool stream_close_context(StreamContext *privateContext);

bool startLogicalStreaming(StreamSpecs *specs);
bool streamCheckResumePosition(StreamSpecs *specs);

bool streamWrite(LogicalStreamContext *context);
bool streamFlush(LogicalStreamContext *context);
bool streamKeepalive(LogicalStreamContext *context);
bool streamClose(LogicalStreamContext *context);
bool streamFeedback(LogicalStreamContext *context);

bool streamRotateFile(LogicalStreamContext *context);
bool streamCloseFile(LogicalStreamContext *context, bool time_to_abort);

bool streamWaitForSubprocess(LogicalStreamContext *context);

bool prepareMessageMetadataFromContext(LogicalStreamContext *context);
bool prepareMessageJSONbuffer(LogicalStreamContext *context);

bool parseMessageActionAndXid(LogicalStreamContext *context);

bool parseMessageMetadata(LogicalMessageMetadata *metadata,
						  const char *buffer,
						  JSON_Value *json,
						  bool skipAction);

bool stream_read_file(StreamContent *content);
bool stream_read_latest(StreamSpecs *specs, StreamContent *content);

bool buildReplicationURI(const char *pguri, char *repl_pguri);

bool stream_setup_databases(CopyDataSpec *copySpecs,
							StreamOutputPlugin plugin,
							char *slotName,
							char *origin);

bool stream_cleanup_databases(CopyDataSpec *copySpecs,
							  char *slotName,
							  char *origin);

bool stream_create_repl_slot(CopyDataSpec *copySpecs,
							 StreamOutputPlugin plugin,
							 char *slotName,
							 uint64_t *lsn);

bool stream_create_origin(CopyDataSpec *copySpecs,
						  char *nodeName, uint64_t startpos);

bool stream_create_sentinel(CopyDataSpec *copySpecs,
							uint64_t startpos,
							uint64_t endpos);

bool stream_write_context(StreamSpecs *specs, LogicalStreamClient *stream);
bool stream_cleanup_context(StreamSpecs *specs);
bool stream_read_context(CDCPaths *paths,
						 IdentifySystem *system,
						 uint32_t *WalSegSz);

StreamAction StreamActionFromChar(char action);

/* ld_transform.c */
bool stream_transform_start_worker(LogicalStreamContext *context);
bool stream_transform_worker(LogicalStreamContext *context);
bool stream_transform_add_file(Queue *queue, uint64_t firstLSN);
bool stream_transform_send_stop(Queue *queue);
bool stream_compute_pathnames(LogicalStreamContext *context, uint64_t lsn);

bool stream_transform_file(char *jsonfilename, char *sqlfilename);
bool stream_write_transaction(FILE *out, LogicalTransaction *tx);
bool stream_write_begin(FILE *out, LogicalTransaction *tx);
bool stream_write_commit(FILE *out, LogicalTransaction *tx);
bool stream_write_switchwal(FILE *out, LogicalMessageSwitchWAL *switchwal);
bool stream_write_keepalive(FILE *out, LogicalMessageKeepalive *keepalive);
bool stream_write_insert(FILE *out, LogicalMessageInsert *insert);
bool stream_write_truncate(FILE *out, LogicalMessageTruncate *truncate);
bool stream_write_update(FILE *out, LogicalMessageUpdate *update);
bool stream_write_delete(FILE * out, LogicalMessageDelete *delete);
bool stream_write_value(FILE *out, LogicalMessageValue *value);

bool parseMessage(LogicalTransaction *txn,
				  LogicalMessageMetadata *metadata,
				  char *message,
				  JSON_Value *json);

bool streamLogicalTransactionAppendStatement(LogicalTransaction *txn,
											 LogicalTransactionStatement *stmt);

void FreeLogicalTransaction(LogicalTransaction *tx);
void FreeLogicalMessageTupleArray(LogicalMessageTupleArray *tupleArray);

/* ld_test_decoding.c */
bool prepareTestDecodingMessage(LogicalStreamContext *context);

bool parseTestDecodingMessageActionAndXid(LogicalStreamContext *context);

bool parseTestDecodingMessage(LogicalTransactionStatement *stmt,
							  LogicalMessageMetadata *metadata,
							  char *message,
							  JSON_Value *json);

/* ld_wal2json.c */
bool prepareWal2jsonMessage(LogicalStreamContext *context);

bool parseWal2jsonMessageActionAndXid(LogicalStreamContext *context);

bool parseWal2jsonMessage(LogicalTransactionStatement *stmt,
						  LogicalMessageMetadata *metadata,
						  char *message,
						  JSON_Value *json);

/* ld_apply.c */
bool stream_apply_catchup(StreamSpecs *specs);

bool stream_apply_wait_for_sentinel(StreamSpecs *specs,
									StreamApplyContext *context);

bool stream_apply_sync_sentinel(StreamApplyContext *context);

bool stream_apply_file(StreamApplyContext *context);

bool setupReplicationOrigin(StreamApplyContext *context,
							CDCPaths *paths,
							char *source_pguri,
							char *target_pguri,
							char *origin,
							uint64_t endpos,
							bool apply);

bool computeSQLFileName(StreamApplyContext *context);

StreamAction parseSQLAction(const char *query, LogicalMessageMetadata *metadata);


/* follow.c */
bool follow_start_prefetch(StreamSpecs *specs, pid_t *pid);
bool follow_start_catchup(StreamSpecs *specs, pid_t *pid);
bool follow_wait_subprocesses(StreamSpecs *specs, pid_t prefetch, pid_t catchup);
bool follow_wait_pid(pid_t subprocess, bool *exited, int *returnCode);

#endif /* LD_STREAM_H */
