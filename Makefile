# pg-memleak-analyzer/Makefile

MODULE_big = memleak_analyzer
EXTENSION  = memleak_analyzer
DATA       = memleak_analyzer--1.0.sql
OBJS	   = src/analyzer_main.o \
			 src/analyzer_core.o \
			 src/analyzer_backend.o \
			 src/analyzer_bgw.o \
			 src/analyzer_test.o

PG_CPPFLAGS = -Iinclude

REGRESS	     = test_suite
REGRESS_OPTS = --load-extension=memleak_analyzer

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)