# first(n) and last(n) on a yield receiver -- the argc=1 slice form, which
# ty_recv_builtin_result's existing first/last arm (argc=0, the single-
# element form) does not cover: it sits inside types.c's `if (argc == 0)`
# block, so an argc=1 call fell through unanswered.
def t_first = yield.first(2)
p t_first { [1, 2, 3] }
p t_first { ["a", "b", "c"] }

def t_last = yield.last(2)
p t_last { [1, 2, 3] }
p t_last { ["a", "b", "c"] }
