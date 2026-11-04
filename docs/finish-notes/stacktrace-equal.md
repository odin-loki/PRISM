# stacktrace equality

`operator==` on `basic_stacktrace` is true for two empty traces and does
not read a stored address. Different sizes compare unequal and do not read
past the shorter trace. The same size compares the stored addresses.
`operator!=` is the negation and does not read an address itself. No entry
is built.

The conformance tasks `cxx_stacktrace_true` and `stacktrace_empty_true` /
`stacktrace_current_bound_true` name that comparison. Their expected
verdicts are unchanged. This is not PROVED and not CLEAN.
