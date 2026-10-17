# clamp(range) (the 1-argument Range form) on a yield receiver. Confirmed
# empirically (--emit-types on direct, non-yield calls) the only case that
# answers the receiver's own kind is an Integer receiver with a plain,
# non-mixed-bound Range; every other combination (a Float receiver with
# any Range, or either receiver with a mixed-bound or Float..Float
# literal) answers poly. A Range literal's own left/right bound kinds
# (read directly off the argument node, the same way its own top-level
# kind already is) are what tell the mixed case apart from the plain one
# at an Integer-receiver site.
def t_same_kind = yield.clamp(0..5)
p t_same_kind { 3 }
p t_same_kind { 3.0 }

# Two Integer-receiver sites (differing only in whether clamping actually
# triggers) plus a Float-receiver site: the Float site always answers
# poly regardless of range_has_float_bound, but an Integer site only
# does because the bound is mixed -- if range_has_float_bound were
# wrongly computed as 0, an Integer site would answer plain Integer
# instead of poly, a type mismatch against the Float site that fails to
# compile (the diagnostic this test exists to force).
def t_mixed_bound = yield.clamp(0..5.0)
p t_mixed_bound { 3 }
p t_mixed_bound { 7 }
p t_mixed_bound { 3.0 }
