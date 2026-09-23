// Generated from grammars/*.gbnf. Keep byte-identical (tests/test_ai.py).
constexpr const char* GBNF_INVARIANTS = R"GBNF(# PRISM loop-invariant candidates: a JSON list of C boolean expressions.
# At run time PRISM replaces the ident rule with the loop's variable names,
# so the model can only name variables that exist. Output is re-validated
# after decoding; anything else is rejected and logged (ai_audit.jsonl).
root   ::= "[" ws ( inv ( ws "," ws inv ){0,15} )? ws "]" ws
inv    ::= "\"" expr "\""
expr   ::= term ( ws binop ws term ){0,8}
term   ::= "!"? atom | "-" atom
atom   ::= ident | number | "(" ws expr ws ")"
binop  ::= "+" | "-" | "*" | "/" | "%" | "<=" | ">=" | "<" | ">" | "==" | "!=" | "&&" | "||"
number ::= [0-9] [0-9]{0,9}
ident  ::= [a-zA-Z_] [a-zA-Z0-9_]{0,31}
ws     ::= [ ]{0,2}
)GBNF";
constexpr const char* GBNF_HARNESS = R"GBNF(# PRISM harness draft: assumptions for a pointer-parameter function.
# nonnull: the pointer is not NULL. size: the pointer addresses exactly
# `elements` elements (a scalar parameter name or a literal). range: an
# integer parameter lies in [lo, hi]. Every assumption is listed in the
# report and the verdict is PROVED-ASSUMING at best, never PROVED.
# At run time the ident rule is replaced with the function's parameter names.
root       ::= "{" ws "\"assumptions\"" ws ":" ws "[" ws ( assumption ( ws "," ws assumption ){0,11} )? ws "]" ws "}" ws
assumption ::= nonnull | size | range
nonnull    ::= "{" ws "\"kind\"" ws ":" ws "\"nonnull\"" ws "," ws "\"param\"" ws ":" ws "\"" ident "\"" ws "}"
size       ::= "{" ws "\"kind\"" ws ":" ws "\"size\"" ws "," ws "\"param\"" ws ":" ws "\"" ident "\"" ws "," ws "\"elements\"" ws ":" ws ( "\"" ident "\"" | number ) ws "}"
range      ::= "{" ws "\"kind\"" ws ":" ws "\"range\"" ws "," ws "\"param\"" ws ":" ws "\"" ident "\"" ws "," ws "\"lo\"" ws ":" ws int ws "," ws "\"hi\"" ws ":" ws int ws "}"
int        ::= "-"? number
number     ::= [0-9] [0-9]{0,5}
ident      ::= [a-zA-Z_] [a-zA-Z0-9_]{0,31}
ws         ::= [ ]{0,2}
)GBNF";
constexpr const char* GBNF_CONTRACT = R"GBNF(# PRISM contract draft (ACSL-ish): requires / ensures clauses over the
# function's parameters and \result. Drafted contracts stay HYPOTHESIS until
# a human approves them. At run time the ident rule is replaced with the
# parameter names. Each clause may end with a trace link to where it came
# from: a requirement sentence id (R12, from --requirements), "comment" (the
# function's comments / header documentation) or "code" (the body itself).
root   ::= clause ( "\n" clause ){0,7} "\n"?
clause ::= ( "requires" | "ensures" ) " " expr ";" trace?
trace  ::= " // from " ( "R" [1-9] [0-9]{0,3} | "comment" | "code" )
expr   ::= term ( " " binop " " term ){0,8}
term   ::= "!"? atom | "-" atom
atom   ::= ident | "\\result" | number | "(" expr ")"
binop  ::= "+" | "-" | "*" | "/" | "%" | "<=" | ">=" | "<" | ">" | "==" | "!=" | "&&" | "||" | "==>"
number ::= [0-9] [0-9]{0,9}
ident  ::= [a-zA-Z_] [a-zA-Z0-9_]{0,31}
)GBNF";
constexpr const char* GBNF_EXPLAIN = R"GBNF(# PRISM counterexample explanation: plain-language explanation plus an
# optional replacement function body. The explanation is HYPOTHESIS (READS);
# the fix is shown as "verified fix" only when BMC proves the patched function.
root  ::= "{" ws "\"explanation\"" ws ":" ws str ws "," ws "\"fix_body\"" ws ":" ws str ws "}" ws
str   ::= "\"" char{0,2000} "\""
char  ::= [^"\\\x00-\x1f] | "\\" ["\\/bfnrt]
ws    ::= [ ]{0,2}
)GBNF";
constexpr const char* GBNF_LEAN_PROOF = R"GBNF(# PRISM Lean proof candidate (roadmap 9.2): a JSON object with the tactic
# lines that replace one `sorry`, in order. A partial list is allowed (the
# search keeps it when Lean reports only "unsolved goals"). The Lean kernel
# checks every candidate, `#print axioms` must stay within propext /
# Classical.choice / Quot.sound, and `lake build` gates acceptance. The
# decoded text is validated again: commands and escape hatches (sorry,
# admit, native_decide, #eval, set_option, axiom, run_tac, ...) are rejected.
root   ::= "{" ws "\"tactics\"" ws ":" ws "[" ws line ( ws "," ws line ){0,39} ws "]" ws "}" ws
line   ::= "\"" char{1,400} "\""
char   ::= [^"\\\x00-\x1f] | "\\" ( "\"" | "\\" )
ws     ::= [ \n]{0,2}
)GBNF";
constexpr const char* GBNF_ASSUMPTION_AUDIT = R"GBNF(# PRISM assumption audit (roadmap 9.3): an independent review of harness
# assumptions and contract `requires`. Each flag names the assumption by
# its index in the list PRISM gave, a realistic input it would exclude, and
# why. Flags are READS findings for the human reviewer; they never change a
# verdict.
root   ::= "{" ws "\"flags\"" ws ":" ws "[" ws ( flag ( ws "," ws flag ){0,15} )? ws "]" ws "}" ws
flag   ::= "{" ws "\"index\"" ws ":" ws number ws "," ws "\"input\"" ws ":" ws str ws "," ws "\"reason\"" ws ":" ws str ws "}"
str    ::= "\"" char{0,300} "\""
char   ::= [^"\\\x00-\x1f] | "\\" ( "\"" | "\\" )
number ::= [0-9] [0-9]{0,2}
ws     ::= [ \n]{0,2}
)GBNF";
