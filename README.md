tcl-libclang
============

Tcl language binding of libclang (work in progress)

Build Procedure
===============

To build this library, the followings programs are necessary.  Install
them first.

- aspell
- fop
- libclang
- tclsh8.6
- xmllint
- xsltproc
- DocBook 5.0
- DocBook 5.0 style sheet

Makefile in the same directory as README.md has the following variable
definitions just after the header comment block.  Change them if it's
necessary.

TCLSH		= /usr/local/bin/tclsh8.6
FOP		= /usr/local/bin/fop
XSLTPROC	= /usr/local/bin/xsltproc
XMLLINT		= /usr/local/bin/xmllint
CFLAGS		= -DBIST -Wall -g -fPIC \
		-I /usr/local/include -I /usr/local/include/tcl8.6/
DOCBOOK_RNG	= ../docbook-5.0/rng/docbook.rng
DOCBOOK_XSL	= ../docbook-xsl-ns-1.78.1

Cd to the directory of README.md.  Run "make".  The following files
will be built in the current directory.

- libcindex.so
- refman.html

Copy libcindex.so to a directory in Tcl library path ($TCLLIBPATH) and
append the contents of pkgIndex.tcl in the current directory to the
directory in Tcl library path.

Run the following Tcl command to load the extension.

$ package require cindex 

Test Procedure
==============

Set PATH so that tcl8.6 can be found in the PATH.
Run "make test".
