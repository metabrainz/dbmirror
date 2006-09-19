###########################################################################
# Makefile for pending.c
# Builds a shared library for postgresql to handling mirroring.
# $Id: Makefile,v 1.2 2006/09/19 02:28:44 ssinger Exp $
#
INCLUDEDIR:=`pg_config --includedir`
SERVERINCLUDEDIR:=`pg_config --includedir-server`
CCFLAGS = -fpic -I${INCLUDEDIR} -I${SERVERINCLUDEDIR}
all:
	gcc $(CCFLAGS) -c  pending.c 
	ld -shared -o pending.so pending.o
