--\echo Use "CREATE EXTENSION bdr" to load this file. \quit

--CREATE ROLE bdr NOLOGIN SUPERUSER;
--SET ROLE bdr;

CREATE SCHEMA bdr;
GRANT USAGE ON SCHEMA bdr TO public;

SET LOCAL search_path = bdr;
-- We must be able to use exclusion constraints for global sequences
SET bdr.permit_unsafe_ddl_commands=true;

CREATE OR REPLACE FUNCTION bdr_version()
RETURNS TEXT
LANGUAGE C
AS 'MODULE_PATHNAME'
;

CREATE FUNCTION pg_stat_get_bdr(
    OUT rep_node_id oid,
    OUT rilocalid oid,
    OUT riremoteid text,
    OUT nr_commit int8,
    OUT nr_rollback int8,
    OUT nr_insert int8,
    OUT nr_insert_conflict int8,
    OUT nr_update int8,
    OUT nr_update_conflict int8,
    OUT nr_delete int8,
    OUT nr_delete_conflict int8,
    OUT nr_disconnect int8
)
RETURNS SETOF record
LANGUAGE C
AS 'MODULE_PATHNAME';

REVOKE ALL ON FUNCTION pg_stat_get_bdr() FROM PUBLIC;

CREATE VIEW pg_stat_bdr AS SELECT * FROM pg_stat_get_bdr();


CREATE TABLE bdr_sequence_values
(
    owning_sysid text NOT NULL,
    owning_tlid oid NOT NULL,
    owning_dboid oid NOT NULL,
    owning_riname text NOT NULL,

    seqschema text NOT NULL,
    seqname text NOT NULL,
    seqrange int8range NOT NULL,

    -- could not acquire chunk
    failed bool NOT NULL DEFAULT false,

    -- voting successfull
    confirmed bool NOT NULL,

    -- empty, not referenced
    emptied bool NOT NULL CHECK(NOT emptied OR confirmed),

    -- used in sequence
    in_use bool NOT NULL CHECK(NOT in_use OR confirmed),

    EXCLUDE USING gist(seqschema WITH =, seqname WITH =, seqrange WITH &&) WHERE (confirmed),
    PRIMARY KEY(owning_sysid, owning_tlid, owning_dboid, owning_riname, seqschema, seqname, seqrange)
);
REVOKE ALL ON TABLE bdr_sequence_values FROM PUBLIC;
SELECT pg_catalog.pg_extension_config_dump('bdr_sequence_values', '');

CREATE INDEX bdr_sequence_values_chunks ON bdr_sequence_values(seqschema, seqname, seqrange);
CREATE INDEX bdr_sequence_values_newchunk ON bdr_sequence_values(seqschema, seqname, upper(seqrange));

CREATE TABLE bdr_sequence_elections
(
    owning_sysid text NOT NULL,
    owning_tlid oid NOT NULL,
    owning_dboid oid NOT NULL,
    owning_riname text NOT NULL,
    owning_election_id bigint NOT NULL,

    seqschema text NOT NULL,
    seqname text NOT NULL,
    seqrange int8range NOT NULL,

    /* XXX id */

    vote_type text NOT NULL,

    open bool NOT NULL,
    success bool NOT NULL DEFAULT false,

    PRIMARY KEY(owning_sysid, owning_tlid, owning_dboid, owning_riname, seqschema, seqname, seqrange)
);
REVOKE ALL ON TABLE bdr_sequence_values FROM PUBLIC;
SELECT pg_catalog.pg_extension_config_dump('bdr_sequence_elections', '');

CREATE TABLE bdr_votes
(
    vote_sysid text NOT NULL,
    vote_tlid oid NOT NULL,
    vote_dboid oid NOT NULL,
    vote_riname text NOT NULL,
    vote_election_id bigint NOT NULL,

    voter_sysid text NOT NULL,
    voter_tlid oid NOT NULL,
    voter_dboid bigint NOT NULL,
    voter_riname text NOT NULL,

    vote bool NOT NULL,
    reason text CHECK (reason IS NULL OR vote = false),
    UNIQUE(vote_sysid, vote_tlid, vote_dboid, vote_riname, vote_election_id, voter_sysid, voter_tlid, voter_dboid, voter_riname)
);
REVOKE ALL ON TABLE bdr_votes FROM PUBLIC;
SELECT pg_catalog.pg_extension_config_dump('bdr_votes', '');

CREATE OR REPLACE FUNCTION bdr_sequence_alloc(INTERNAL)
RETURNS INTERNAL
LANGUAGE C
STABLE STRICT
AS 'MODULE_PATHNAME'
;

CREATE OR REPLACE FUNCTION bdr_sequence_setval(INTERNAL)
RETURNS INTERNAL
LANGUAGE C
STABLE STRICT
AS 'MODULE_PATHNAME'
;

CREATE OR REPLACE FUNCTION bdr_sequence_options(INTERNAL)
RETURNS INTERNAL
LANGUAGE C
STABLE STRICT
AS 'MODULE_PATHNAME'
;

-- not tracked yet, can we trick pg_depend instead?
DELETE FROM pg_seqam WHERE seqamname = 'bdr';

INSERT INTO pg_seqam(
    seqamname,
    seqamalloc,
    seqamsetval,
    seqamoptions
)
VALUES (
    'bdr',
    'bdr_sequence_alloc',
    'bdr_sequence_setval',
    'bdr_sequence_options'
);

CREATE TYPE bdr_conflict_type AS ENUM
(
    'insert_insert',
    'insert_update',
    'update_update',
    'update_delete',
    'unhandled_tx_abort'
);

COMMENT ON TYPE bdr_conflict_type IS 'The nature of a BDR apply conflict - concurrent updates (update_update), conflicting inserts, etc.';

CREATE TYPE bdr.bdr_conflict_handler_action
    AS ENUM('IGNORE', 'ROW', 'SKIP');

CREATE TABLE bdr.bdr_conflict_handlers (
    ch_name NAME NOT NULL,
    ch_type bdr.bdr_conflict_type NOT NULL,
    ch_reloid Oid NOT NULL,
    ch_fun regprocedure NOT NULL,
    ch_timeframe INTERVAL,
    PRIMARY KEY(ch_reloid, ch_name)
) WITH OIDS;
REVOKE ALL ON TABLE bdr_conflict_handlers FROM PUBLIC;
SELECT pg_catalog.pg_extension_config_dump('bdr_conflict_handlers', '');

CREATE INDEX bdr_conflict_handlers_ch_type_reloid_idx
    ON bdr_conflict_handlers(ch_reloid, ch_type);

CREATE FUNCTION bdr.bdr_create_conflict_handler(
    ch_rel REGCLASS,
    ch_name NAME,
    ch_proc REGPROCEDURE,
    ch_type bdr.bdr_conflict_type,
    ch_timeframe INTERVAL
)
RETURNS VOID
LANGUAGE C
STRICT
AS 'MODULE_PATHNAME'
;

CREATE FUNCTION bdr.bdr_create_conflict_handler(
    ch_rel REGCLASS,
    ch_name NAME,
    ch_proc REGPROCEDURE,
    ch_type bdr.bdr_conflict_type
)
RETURNS VOID
LANGUAGE C
STRICT
AS 'MODULE_PATHNAME'
;

CREATE FUNCTION bdr.bdr_drop_conflict_handler(ch_rel REGCLASS, ch_name NAME)
RETURNS VOID
LANGUAGE C
STRICT
AS 'MODULE_PATHNAME'
;

CREATE VIEW bdr_list_conflict_handlers(ch_name, ch_type, ch_reloid, ch_fun) AS
    SELECT ch_name, ch_type, ch_reloid, ch_fun, ch_timeframe
    FROM bdr.bdr_conflict_handlers
;


CREATE TYPE bdr_conflict_resolution AS ENUM
(
    'conflict_trigger_skip_change',
    'conflict_trigger_returned_tuple',
    'last_update_wins_keep_local',
    'last_update_wins_keep_remote',
    'unhandled_tx_abort'
);

COMMENT ON TYPE bdr_conflict_resolution IS 'Resolution of a bdr conflict - if a conflict was resolved by a conflict trigger, by last-update-wins tests on commit timestamps, etc.';

--
-- bdr_conflict_history records apply conflicts so they can be queried and
-- analysed by administrators.
--
-- This must remain in sync with bdr_log_handled_conflict(...) and
-- struct BdrApplyConflict
--
CREATE SEQUENCE bdr_conflict_history_id_seq;

CREATE TABLE bdr_conflict_history (
    conflict_id         bigint not null default nextval('bdr_conflict_history_id_seq'),
    local_node_sysid    text not null, -- really uint64 but we don't have the type for it
    PRIMARY KEY (local_node_sysid, conflict_id),

    local_conflict_xid  xid not null,     -- xid of conflicting apply tx
    local_conflict_lsn  pg_lsn not null,  -- lsn of local node at the time the conflict was detected
    local_conflict_time timestamptz not null,
    object_schema       text,
    object_name         text,
    remote_node_sysid   text not null, -- again, really uint64
    remote_txid         xid not null,
    remote_commit_time  timestamptz not null,
    remote_commit_lsn   pg_lsn not null,
    conflict_type       bdr_conflict_type not null,
    conflict_resolution bdr_conflict_resolution not null,
    local_tuple         json,
    remote_tuple        json,
    local_tuple_xmin    xid,
    local_tuple_origin_sysid text,        -- also really uint64

    -- The following apply only for unhandled apply errors and
    -- correspond to fields in ErrorData in elog.h .
    error_message       text,
    error_sqlstate      text CHECK (length(error_sqlstate) = 5),
    error_querystring   text,
    error_cursorpos     integer,
    error_detail        text,
    error_hint          text,
    error_context       text,
    error_columnname    text, -- schema and table in object_schema, object_name above
    error_typename      text,
    error_constraintname text,
    error_filename      text,
    error_lineno        integer,
    error_funcname      text
);
REVOKE ALL ON TABLE bdr_conflict_history FROM PUBLIC;
SELECT pg_catalog.pg_extension_config_dump('bdr_conflict_history', '');

ALTER SEQUENCE bdr_conflict_history_id_seq OWNED BY bdr_conflict_history.conflict_id;

COMMENT ON TABLE bdr_conflict_history IS 'Log of all conflicts in this BDR group';
COMMENT ON COLUMN bdr_conflict_history.local_node_sysid IS 'sysid of the local node where the apply conflict occurred';
COMMENT ON COLUMN bdr_conflict_history.remote_node_sysid IS 'sysid of the remote node the conflicting transaction originated from';
COMMENT ON COLUMN bdr_conflict_history.object_schema IS 'Schema of the object involved in the conflict';
COMMENT ON COLUMN bdr_conflict_history.object_name IS 'Name of the object (table, etc) involved in the conflict';
COMMENT ON COLUMN bdr_conflict_history.local_conflict_xid IS 'Transaction ID of the apply transaction that encountered the conflict';
COMMENT ON COLUMN bdr_conflict_history.local_conflict_lsn IS 'xlog position at the time the conflict occured on the applying node';
COMMENT ON COLUMN bdr_conflict_history.local_conflict_time IS 'The time the conflict was detected on the applying node';
COMMENT ON COLUMN bdr_conflict_history.remote_txid IS 'xid of the remote transaction involved in the conflict';
COMMENT ON COLUMN bdr_conflict_history.remote_commit_time IS 'The time the remote transaction involved in this conflict committed';
COMMENT ON COLUMN bdr_conflict_history.remote_commit_lsn IS 'LSN on remote node at which conflicting transaction committed';
COMMENT ON COLUMN bdr_conflict_history.conflict_type IS 'Nature of the conflict - insert/insert, update/delete, etc';
COMMENT ON COLUMN bdr_conflict_history.local_tuple IS 'For DML conflicts, the conflicting tuple from the local DB (as json), if logged';
COMMENT ON COLUMN bdr_conflict_history.local_tuple_xmin IS 'If local_tuple is set, the xmin of the conflicting local tuple';
COMMENT ON COLUMN bdr_conflict_history.local_tuple_origin_sysid IS 'The node id for the true origin of the local tuple. Differs from local_node_sysid if the tuple was originally replicated from another node.';
COMMENT ON COLUMN bdr_conflict_history.remote_tuple IS 'For DML conflicts, the conflicting tuple from the remote DB (as json), if logged';
COMMENT ON COLUMN bdr_conflict_history.conflict_resolution IS 'How the conflict was resolved/handled; see the enum definition';
COMMENT ON COLUMN bdr_conflict_history.error_message IS 'On apply error, the error message from ereport/elog. Other error fields match.';

-- The bdr_nodes table tracks members of a BDR group; it's only concerned with
-- one bdr group so it only has to track enough to uniquely identify each member
-- node, which is the (sysid, timeline, dboid) tuple for that node.
--
-- The sysid must be a numeric (or string) because PostgreSQL has no uint64 SQL
-- type.
--
CREATE TABLE bdr_nodes (
    node_sysid text not null, -- Really a uint64 but we have no type for that
    node_timeline oid not null,
    node_dboid oid not null,  -- This is an oid local to the node_sysid cluster
    node_status "char" not null,
    primary key(node_sysid, node_timeline, node_dboid),
    check (node_status in ('i', 'c', 'r'))
);
REVOKE ALL ON TABLE bdr_nodes FROM PUBLIC;
SELECT pg_catalog.pg_extension_config_dump('bdr_nodes', '');

COMMENT ON TABLE bdr_nodes IS 'All known nodes in this BDR group.';
COMMENT ON COLUMN bdr_nodes.node_sysid IS 'system_identifier from the control file of the node';
COMMENT ON COLUMN bdr_nodes.node_timeline IS 'timeline ID of this node';
COMMENT ON COLUMN bdr_nodes.node_dboid IS 'local database oid on the cluster (node_sysid, node_timeline)';
COMMENT ON COLUMN bdr_nodes.node_status IS 'Readiness of the node: [i]nitializing, [c]atchup, [r]eady. Doesn''t indicate connected/disconnected.';

CREATE TABLE bdr_global_locks(
    locktype text NOT NULL,

    owning_sysid text NOT NULL,
    owning_timeline oid NOT NULL,
    owning_datid oid NOT NULL,

    owner_created_lock_at pg_lsn NOT NULL,

    acquired_sysid text NOT NULL,
    acquired_timeline oid NOT NULL,
    acquired_datid oid NOT NULL,

    acquired_lock_at pg_lsn,

    state text NOT NULL
);
REVOKE ALL ON TABLE bdr_global_locks FROM PUBLIC;
SELECT pg_catalog.pg_extension_config_dump('bdr_global_locks', '');

CREATE UNIQUE INDEX bdr_global_locks_byowner
ON bdr_global_locks(locktype, owning_sysid, owning_timeline, owning_datid);

CREATE TABLE bdr_queued_commands (
    lsn pg_lsn NOT NULL,
    queued_at TIMESTAMP WITH TIME ZONE NOT NULL,
    perpetrator TEXT NOT NULL,
    command_tag TEXT NOT NULL,
    command TEXT NOT NULL
);
REVOKE ALL ON TABLE bdr_queued_commands FROM PUBLIC;
SELECT pg_catalog.pg_extension_config_dump('bdr_queued_commands', '');

CREATE OR REPLACE FUNCTION bdr.queue_truncate()
    RETURNS TRIGGER
    LANGUAGE plpgsql
    AS $function$
DECLARE
    ident TEXT;
BEGIN
    -- don't recursively log truncation commands
    IF pg_replication_identifier_is_replaying() THEN
       RETURN NULL;
    END IF;

    ident := quote_ident(TG_TABLE_SCHEMA)||'.'||quote_ident(TG_TABLE_NAME);

    INSERT INTO bdr.bdr_queued_commands (
        lsn, queued_at, perpetrator,
        command_tag, command
    )
    VALUES (
        pg_current_xlog_location(),
        NOW(), CURRENT_USER,
        'TRUNCATE (automatic)',
        'TRUNCATE TABLE ONLY ' || ident
        );
    RETURN NULL;
END;
$function$;

CREATE OR REPLACE FUNCTION bdr.bdr_queue_ddl_commands()
RETURNS event_trigger
LANGUAGE C
AS 'MODULE_PATHNAME'
;

-- This type is tailored to use as input to get_object_address
CREATE TYPE bdr.dropped_object AS (
    objtype text,
    objnames text[],
    objargs text[]
);

CREATE TABLE bdr.bdr_queued_drops (
    lsn pg_lsn NOT NULL,
    queued_at timestamptz NOT NULL,
    dropped_objects bdr.dropped_object[] NOT NULL
);
REVOKE ALL ON TABLE bdr_queued_drops FROM PUBLIC;
SELECT pg_catalog.pg_extension_config_dump('bdr_queued_drops', '');

CREATE OR REPLACE FUNCTION bdr.queue_dropped_objects()
RETURNS event_trigger
LANGUAGE plpgsql
AS $function$
DECLARE
    r RECORD;
    dropped bdr.dropped_object;
    otherobjs bdr.dropped_object[] = '{}';
BEGIN
    -- don't recursively log drop commands
    IF pg_replication_identifier_is_replaying() THEN
       RETURN;
    END IF;

    FOR r IN SELECT * FROM pg_event_trigger_dropped_objects()
    LOOP
        IF r.original OR r.normal THEN
            dropped.objtype = r.object_type;
            dropped.objnames = r.address_names;
            dropped.objargs = r.address_args;
            otherobjs := otherobjs || dropped;
            RAISE LOG 'object is: %', dropped;
        END IF;
    END LOOP;

    IF otherobjs <> '{}' THEN
        INSERT INTO bdr.bdr_queued_drops (
            lsn, queued_at, dropped_objects
        )
        VALUES (pg_current_xlog_location(),
            NOW(),
            otherobjs
        );
    END IF;
END;
$function$;

CREATE EVENT TRIGGER queue_drops
ON sql_drop
EXECUTE PROCEDURE bdr.queue_dropped_objects();

CREATE OR REPLACE FUNCTION bdr_apply_pause()
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME'
;

CREATE OR REPLACE FUNCTION bdr_apply_resume()
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME'
;

---
--- this should always be last to avoid replicating our internal schema
---

CREATE EVENT TRIGGER bdr_queue_ddl_commands
ON ddl_command_end
EXECUTE PROCEDURE bdr.bdr_queue_ddl_commands();

SET bdr.permit_unsafe_ddl_commands = false;
RESET search_path;
