tcl-libclang
============

Tcl language binding of libclang (work in progress)

Build Procedure
===============

# Building on UNIX like systems

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

# Building on Windows

At first, make sure that your Tcl installation has been built with
64-bit VC compiler.

~~~
% puts $tcl_platform(pointerSize)
8
~~~

If the result is not '8', rebuild Tcl with 64-bit VC compiler.

Modify the following line of win/makefile.vc if you have installed
LLVM on a different path.  *The path must not include any spaces.*
Note that the default installation path of LLVM is 'C:\Program
Files\LLVM' and our makefile can't handle it.

~~~
LLVM_PATH = "c:\LLVM"
~~~

Open a *x64* Native Tools Command Prompt and run the following command
in 'win/'.

~~~
> nmake -f makefile.vc INSTALLDIR=<your Tcl install path>
> nmake -f makefile.vc INSTALLDIR=<your Tcl install path> install
~~~

Test Procedure
==============

Set PATH so that tcl8.6 can be found in the PATH.
Run "make test".
