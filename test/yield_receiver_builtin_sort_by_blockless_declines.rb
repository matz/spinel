# sort_by's row requires a block (BF_REQUIRED). The generic fallback
# always passes has_block=0 to the lookup (it only ever answers a
# blockless use of the yield value, the same precondition both of
# ty_recv_builtin_result's callers already enforce before reaching it),
# so it can never match sort_by's row regardless of whether this call
# itself has a block -- bop_find_arg's own row-fit guard enforces this
# structurally. The call that actually reaches this path is the
# blockless form (answers an Enumerator, not sort_by's own BOPR_SELF),
# proving the generic fallback declines it rather than wrongly answering
# the receiver's own kind.
def t = yield.sort_by
p(t { [3, 1, 2] }.to_a)
p(t { ["b", "a"] }.to_a)
