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
CFLAGS		= -DBIST -Wall -g -fPIC \
		-I /usr/local/include -I /usr/local/include/tcl8.6
DITAOTI		= ../DITA-OT1.8.4
ANT		= $(DITAOTI)/tools/ant/bin/ant
ANT_OPTS	= -Xmx512m -Djavax.xml.transform.TransformerFactory=net.sf.saxon.TransformerFactoryImpl
ANT_HOME	= $(DITAOTI)/tools/ant
CLASSPATH	= $(DITAOTI)/lib/saxon/saxon9-dom.jar:$(DITAOTI)/lib/saxon/saxon9.jar:$(DITAOTI)/lib/xml-apis.jar:$(DITAOTI)/lib/xercesImpl.jar:$(DITAOTI)/lib/icu4j.jar:$(DITAOTI)/lib/resolver.jar:$(DITAOTI)/lib/commons-codec-1.4.jar:$(DITAOTI)/lib:$(DITAOTI)/lib/dost.jar

default: libcindex.so

obj/libcindex.o: src/libcindex.c
	mkdir -p obj
	$(CC) -c -o obj/libcindex.o $(CFLAGS) src/libcindex.c

lib/libcindex.so: obj/libcindex.o
	$(CC) -shared -o lib/libcindex.so obj/libcindex.o \
		-L /usr/local/lib -ltcl86 -lclang
	chmod -x lib/libcindex.so

doc/refman/refman.html: docsrc/refman.ditamap
	CLASSPATH="$(CLASSPATH)" \
	ANT_OPTS="$(ANT_OPTS)" \
	ANT_HOME="$(ANT_HOME)" \
		$(ANT) -f build-dita.xml \
			-Ddita.dir=$(DITAOTI) \
			-Dargs.input=docsrc/refman.ditamap \
			-Dtranstype=xhtml \
			-Doutput.dir=doc/refman

clean:
	rm -f lib/libcindex.so
	rm -rf obj doc man

test:	lib/libcindex.so
	TCLLIBPATH=lib $(TCLSH) src/cindex.test
