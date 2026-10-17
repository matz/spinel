# pred and div on a yield receiver under --int-overflow=promote: both are
# gated by g_promote_mode in analyze_infer.c, scattered across arms this
# generic fallback's receiver-kind guard must exclude structurally (by
# receiver kind, not by enumerating every promote-gated name) -- an
# earlier version of that guard excluded only succ/sum/to_i/divmod/pow/<<
# by name and missed these two, turning a correct promote-mode answer
# into a RangeError.
def t_pred = yield.pred
p t_pred { 3 }
p t_pred { -9223372036854775808 }

def t_div = yield.div(1.0)
p t_div { 3 }
p t_div { 2.0**70 }
