# This Makefile serves as a wrapper to bundle the
# pcre2 module without modifications.

MODULES=pcre2
OBJS=pcre2
INTERFACES=pcre2
C_OBJS=pcre2
ARCHIVE=pcre2
LIBRARY=pcre2
C_LIBS=pcre2-8
SRC_DIR=pcre2-ocaml/lib

OCAMLFLAGS+=-I $(STDCOMPATDIR)

EXTRACFLAGS=$(PCRE_CFLAGS:%=-ccopt %) $(PCRE_LIBS:%=-cclib %)
EXTRALNKFLAGS=$(PCRE_LIBS:%=-ldopt %)

include ../Makefile.bundles

.PHONY: distclean
distclean:
