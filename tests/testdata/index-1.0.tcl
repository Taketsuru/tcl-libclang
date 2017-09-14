package require cindex
cindex::index -displayDiagnostics myindex
myindex translationUnit mytu [regsub {tcl$} [info script] {c}]
rename myindex ""
