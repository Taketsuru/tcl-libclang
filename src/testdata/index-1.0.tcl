package require cindex
cindex::index -displayDiagnostics myindex
myindex translationUnit mytu src/testdata/error.c
