package require tcltest

tcltest::testsDirectory [file dir [info script]]
tcltest::configure -tmpdir [expr {[info exists ::env(TMPDIR)]? $::env(TMPDIR) : "/tmp"}]
tcltest::configure {*}$argv;
tcltest::runAllTests
return
