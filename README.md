tcl-libclang
============

Tcl language binding of libclang (work in progress)

Build Procedure
===============

`tcl-libclang` is packaged as a TEA extension.  For example:

~~~{.sh}
./configure --prefix=/usr/local --exec-prefix=/usr/local
make
make install
~~~

If multiple LLVM versions are installed, the `--llvm-config` configure option should be set to the path of the `llvm-config` for the version to be linked.

Please, tweak the configure options `--with-dita-ot`, `--with-xml-impl` and `--with-classpath` to your environment in order to build the documentation.

Run the following Tcl commands to load the extension:

~~~{.tcl}
::lappend ::auto_path [file join / usr local lib]; # the exec_path
package require cindex 
~~~

Test Procedure
==============

Set PATH so that tcl8.6 can be found in the PATH.
Run "make test".
