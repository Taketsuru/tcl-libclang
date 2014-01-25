# ============================================================================
#
# Copyright (c) 2014 Taketsuru <taketsuru11@gmail.com>.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
# this list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
# this list of conditions and the following disclaimer in the documentation
# and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY
# DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
# THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# ============================================================================

TCLSH		= /usr/local/bin/tclsh8.6
FOP		= /usr/local/bin/fop
XSLTPROC	= /usr/local/bin/xsltproc
XMLLINT		= /usr/local/bin/xmllint
CFLAGS		= -DBIST -Wall -g -fPIC \
		-I /usr/local/include -I /usr/local/include/tcl8.6/
DOCBOOK_RNG	= ../docbook-5.0/rng/docbook.rng
DOCBOOK_XSL	= ../docbook-xsl-ns-1.78.1

default: libcindex.so refman.html

obj/libcindex.o: src/libcindex.c
	mkdir -p obj
	$(CC) -c -o obj/libcindex.o $(CFLAGS) src/libcindex.c

libcindex.so: obj/libcindex.o
	$(CC) -shared -o libcindex.so obj/libcindex.o \
		-L /usr/local/lib -ltcl86 -lclang
	chmod -x libcindex.so

obj/refman.valid: doc/en_US/refman.docbook
	mkdir -p obj
	rm -f refman.valid
	$(XMLLINT) --noout --relaxng $(DOCBOOK_RNG) doc/en_US/refman.docbook
	touch obj/refman.valid

refman.html: obj/refman.valid
	$(XSLTPROC) -o refman.html \
		$(DOCBOOK_XSL)/html/docbook.xsl \
		doc/en_US/refman.docbook

clean:
	rm -f libcindex.so refman.html
	rm -rf obj man

test:	libcindex.so
	TCLLIBPATH=. $(TCLSH) src/cindex.test
