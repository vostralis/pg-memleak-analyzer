# pg-memleak-analyzer/Makefile

MODULES   = memleak_analyzer
EXTENSION = memleak_analyzer
DATA      = memleak_analyzer--1.0.sql

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)