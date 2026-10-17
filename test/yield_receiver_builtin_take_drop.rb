# take(n) and drop(n) on a yield receiver: both answer the receiver's own
# Array kind (a slice), never argument-dependent -- their bop_rows row
# answered TY_UNKNOWN only because it was never finished, not because the
# answer depends on anything but the receiver's kind (confirmed empirically
# against a direct, non-yield take(1): dispatch "direct", type int_array).
def t_take = yield.take(1)
p t_take { [1, 2, 3] }
p t_take { ["a", "b", "c"] }

def t_drop = yield.drop(1)
p t_drop { [1, 2, 3] }
p t_drop { ["a", "b", "c"] }
