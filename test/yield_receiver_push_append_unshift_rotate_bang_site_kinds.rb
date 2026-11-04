# push/append/unshift/rotate! on a yield, with an Int Array block at one
# call site and a String Array block at another. Each name's 0-arg (or,
# for rotate!, 0- and 1-arg) row answered BOPR_SELF already for a direct
# call, but was never on yield_site_listed/bop_result_fallback_allowed, so
# the yield stayed typed to its first site alone and a second site of a
# different Array kind failed to compile (an IntArray pointer into a
# StrArray slot or vice versa). kilau K-036.
# CRuby: [3, 1, 2], ["b", "a"], [3, 1, 2], ["b", "a"], [3, 1, 2], ["b", "a"],
# [1, 2, 3], ["a", "b"], [1, 2, 3], ["a", "b"].
def t_push = yield.push
def t_append = yield.append
def t_unshift = yield.unshift
def t_rotate_bang = yield.rotate!
def t_rotate_bang_n = yield.rotate!(1)

p t_push { [3, 1, 2] }
p t_push { ["b", "a"] }
p t_append { [3, 1, 2] }
p t_append { ["b", "a"] }
p t_unshift { [3, 1, 2] }
p t_unshift { ["b", "a"] }
p t_rotate_bang { [3, 1, 2] }
p t_rotate_bang { ["b", "a"] }
p t_rotate_bang_n { [3, 1, 2] }
p t_rotate_bang_n { ["b", "a"] }
