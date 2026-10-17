# clamp(lo, hi) (the 2-argument form) on a yield receiver: has no bop_rows
# row at all for Integer/Float -- its answer is hand-written, split
# across two places (analyze_infer.c's Integer-receiver arm,
# analyze_infer_recv.c's Float-receiver one): an Integer receiver with a
# Float or poly bound is poly, not Integer; a Float receiver is Float
# only when BOTH bounds are Float, poly otherwise (including a poly
# bound) -- either bound nil is poly regardless of receiver kind.
def t = yield.clamp(0, 5)
p t { 3 }
p t { 7.5 }

# Both bounds Float: an Integer receiver's result is boxed (poly), a
# Float receiver's stays Float -- the two cases a fresh review found
# this arm answering wrong before (it only checked a Float receiver's
# bounds, never an Integer receiver's).
def t_float_bounds = yield.clamp(0.0, 5.0)
p t_float_bounds { 7 }
p t_float_bounds { 7.5 }
