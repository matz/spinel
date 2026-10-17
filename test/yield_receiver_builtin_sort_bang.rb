# A sort! on a yield receiver answers per call site, same family as
# K-024/#9419's dig/succ/sum and K-029's other names: the shared table
# ty_recv_builtin_result (src/types.c) must answer this name too, or an
# IntArray block at one site and a StrArray block at another emit
# incompatible C pointer types into the same slot.
def t = yield.sort!
p t { [3, 1, 2] }
p t { ["b", "a"] }
