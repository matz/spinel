# clamp(range) (the 1-argument Range form) on a yield receiver. Confirmed
# empirically (--emit-types on direct, non-yield calls) the only case that
# answers the receiver's own kind is an Integer receiver with a plain,
# non-mixed-bound Range; every other combination (a Float receiver with
# any Range, or either receiver with a mixed-bound or TY_FLOAT_RANGE
# literal) answers poly. range_has_float_bound (computed from the Range
# literal's own left/right children) is what tells the mixed case apart
# from the plain one at the same Integer-receiver site.
def t_same_kind = yield.clamp(0..5)
p t_same_kind { 3 }
p t_same_kind { 3.0 }

def t_mixed_bound = yield.clamp(0..5.0)
p t_mixed_bound { 3 }
p t_mixed_bound { 7 }
