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
# parameter names.
root   ::= clause ( "\n" clause ){0,7} "\n"?
clause ::= ( "requires" | "ensures" ) " " expr ";"
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
