/*-------------------------------------------------------------------------
 * replication_identifier.h
 *     XXX
 *
 * Copyright (c) 2013, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef REPLICATION_IDENTIFIER_H
#define REPLICATION_IDENTIFIER_H

#include "catalog/pg_replication_identifier.h"
#include "replication/logical.h"

/* API for querying & manipulating replication identifiers */
extern RepNodeId GetReplicationIdentifier(char *name, bool missing_ok);
extern RepNodeId CreateReplicationIdentifier(char *name);
extern HeapTuple GetReplicationInfoByIdentifier(RepNodeId riident, bool missing_ok);

extern void AdvanceReplicationIdentifier(RepNodeId node,
										 XLogRecPtr remote_commit,
										 XLogRecPtr local_commit);
extern void AdvanceCachedReplicationIdentifier(XLogRecPtr remote_commit,
											   XLogRecPtr local_commit);
extern void SetupCachedReplicationIdentifier(RepNodeId node);
extern void TeardownCachedReplicationIdentifier(void);
extern XLogRecPtr RemoteCommitFromCachedReplicationIdentifier(void);

/* crash recovery support */
extern void CheckPointReplicationIdentifier(XLogRecPtr ckpt);
extern void StartupReplicationIdentifier(XLogRecPtr ckpt);

/* internals */
extern Size ReplicationIdentifierShmemSize(void);
extern void ReplicationIdentifierShmemInit(void);

/* SQL callable functions */
extern Datum pg_replication_identifier_get(PG_FUNCTION_ARGS);
extern Datum pg_replication_identifier_create(PG_FUNCTION_ARGS);
extern Datum pg_replication_identifier_setup_replaying_from(PG_FUNCTION_ARGS);
extern Datum pg_replication_identifier_reset_replaying_from(PG_FUNCTION_ARGS);
extern Datum pg_replication_identifier_is_replaying(PG_FUNCTION_ARGS);
extern Datum pg_replication_identifier_setup_tx_origin(PG_FUNCTION_ARGS);
extern Datum pg_get_replication_identifier_progress(PG_FUNCTION_ARGS);
extern Datum pg_replication_identifier_advance(PG_FUNCTION_ARGS);

#endif
