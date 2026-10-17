# clamp(lo, hi) (the 2-argument form) on a yield receiver: has no bop_rows
# row at all for Integer/Float -- its answer is 100% hand-written in
# analyze_infer_recv.c, comparing both bounds' own kinds against the
# receiver. Needs a second argument's kind (arg1), which this plan adds as
# a new parameter to ty_recv_builtin_result.
def t = yield.clamp(0, 5)
p t { 3 }
p t { 7.5 }
