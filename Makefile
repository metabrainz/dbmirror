###########################################################################
# Makefile for pending.c
# Builds a shared library for postgresql to handling mirroring.

MODULES = pending
OBJS = pending.o

PGXS := $(shell pg_config --pgxs)
include $(PGXS)

