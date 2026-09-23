// Generated from grammars/{ask,draft}.gbnf. Keep byte-identical (tests/test_ai_assist.py).
constexpr const char* GBNF_ASK = R"GBNF(# PRISM questions over findings (roadmap 9.4): a natural-language question is
# translated into this structured query. The query is printed with the answer
# so it can be checked; the model never sees findings and cannot set a verdict.
# At run time the stage and status rules are the pipeline's own vocabularies.
root     ::= "{" ws "\"stages\"" ws ":" ws stages ws "," ws "\"statuses\"" ws ":" ws statuses ws "," ws "\"cls\"" ws ":" ws strs ws "," ws "\"file_glob\"" ws ":" ws str ws "," ws "\"function_glob\"" ws ":" ws str ws "," ws "\"text\"" ws ":" ws str ws "," ws "\"group_by\"" ws ":" ws groupby ws "," ws "\"count_only\"" ws ":" ws bool ws "}" ws
stages   ::= "[" ws ( stage ( ws "," ws stage ){0,8} )? ws "]"
statuses ::= "[" ws ( status ( ws "," ws status ){0,12} )? ws "]"
strs     ::= "[" ws ( str ( ws "," ws str ){0,8} )? ws "]"
stage    ::= "\"" [a-z]{2,12} "\""
status   ::= "\"PROVED-CERTIFIED\"" | "\"PROVED-UNBOUNDED\"" | "\"PROVED\"" | "\"PROVED-ASSUMING\"" | "\"BOUNDED\"" | "\"FAILED\"" | "\"UNKNOWN\"" | "\"TIMEOUT\"" | "\"ERROR\"" | "\"NOTRUN\"" | "\"NEEDS-HARNESS\"" | "\"CRASH\"" | "\"CLEAN\"" | "\"SANFAIL\"" | "\"HYPOTHESIS\"" | "\"READS\""
groupby  ::= "\"\"" | "\"stage\"" | "\"status\"" | "\"cls\"" | "\"file\"" | "\"function\""
bool     ::= "true" | "false"
str      ::= "\"" [a-zA-Z0-9_ .*?/+:-]{0,80} "\""
ws       ::= [ ]{0,2}
)GBNF";
constexpr const char* GBNF_DRAFT = R"GBNF(# PRISM report / assurance-case drafting (roadmap 9.4): the model may reword
# the claims of the template draft. Every claim carries links; PRISM's
# validator rejects a claim without a link, or with a link that does not
# resolve to a finding, verdict, theorem or certificate of this run.
root   ::= "[" ws claim ( ws "," ws claim ){0,63} ws "]" ws
claim  ::= "{" ws "\"section\"" ws ":" ws str ws "," ws "\"text\"" ws ":" ws text ws "," ws "\"links\"" ws ":" ws links ws "}"
links  ::= "[" ws link ( ws "," ws link ){0,15} ws "]"
link   ::= "\"" ( "finding:" | "verdict:" | "theorem:" | "certificate:" | "stage:" ) [a-zA-Z0-9_.#'-]{1,120} "\""
str    ::= "\"" [a-zA-Z0-9 ,.()'-]{1,80} "\""
text   ::= "\"" char{1,600} "\""
char   ::= [^"\\\x00-\x1f] | "\\" ["\\/bfnrt]
ws     ::= [ \n]{0,2}
)GBNF";
