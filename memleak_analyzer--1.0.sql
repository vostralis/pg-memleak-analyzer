/* pg-memleak-analyzer/memleak_analyzer--1.0.sql */ 

\echo Use "CREATE EXTENSION memleak_analyzer" to load this file. \quit

CREATE SCHEMA IF NOT EXISTS memleak_analyzer;

CREATE OR REPLACE FUNCTION memleak_analyzer.analyze_query(query text)
RETURNS TABLE (
    context_name     TEXT,
    parent_name      TEXT,
    level            INT,
    allocated_before BIGINT,
    allocated_after  BIGINT,
    delta_bytes      BIGINT,
    leak_suspected   BOOLEAN
)
AS 'MODULE_PATHNAME', 'analyze_query'
LANGUAGE C STRICT VOLATILE;

CREATE OR REPLACE FUNCTION memleak_analyzer.get_bgw_memory_snapshot(target_pid INTEGER)
RETURNS TABLE (
    context_name TEXT,
    parent_name  TEXT,
    level        INT,
    allocated    BIGINT
)
AS 'MODULE_PATHNAME', 'get_bgw_memory_snapshot'
LANGUAGE C STRICT VOLATILE;