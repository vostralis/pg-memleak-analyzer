# pg-memleak-analyzer/Makefile

MODULE_big = memleak_analyzer
EXTENSION  = memleak_analyzer
DATA       = memleak_analyzer--1.0.sql
OBJS	   = analyzer_main.o analyzer_core.o analyzer_backend.o analyzer_bgw.o

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)