# spinel: gc-stress
# More builtins answered per yield-receiver site, closing part of K-029's
# "other names likely follow the same pattern" leftover (kilau K-033):
# clear/flatten/first/shift answer a Hash site too, now that their bop_rows
# row for Hash is concrete (unlike min/max/minmax/uniq, whose Hash row is
# still a placeholder and stays unanswered on purpose); []/slice/delete/
# delete_at/assoc/rassoc/values_at/pop(n)/shift(n) answer across Array sites
# of a different element kind, which no existing arm covered at all.
def t_clear = yield.clear
p t_clear { [1, 2, 3] }
p t_clear { ["a", "b"] }
p t_clear { { a: 1 } }

def t_flatten = yield.flatten
p t_flatten { [1, 2, 3] }
p t_flatten { ["a", "b"] }
p t_flatten { { a: 1 } }

def t_first = yield.first
p t_first { [1, 2, 3] }
p t_first { { a: 1 } }

def t_shift = yield.shift
p t_shift { [1, 2, 3] }
p t_shift { { a: 1 } }

def t_values_at = yield.values_at(1)
p t_values_at { [1, 2, 3] }
p t_values_at { ["a", "b"] }

def t_aref = yield.[](1)
p t_aref { [1, 2, 3] }
p t_aref { ["a", "b"] }

def t_slice = yield.slice(1)
p t_slice { [1, 2, 3] }
p t_slice { ["a", "b"] }

def t_delete = yield.delete(1)
p t_delete { [1, 2, 3] }
p t_delete { ["a", "b"] }

def t_delete_at = yield.delete_at(0)
p t_delete_at { [1, 2, 3] }
p t_delete_at { ["a", "b"] }

def t_assoc = yield.assoc(1)
p t_assoc { [1, 2, 3] }
p t_assoc { ["a", "b"] }

def t_rassoc = yield.rassoc(1)
p t_rassoc { [1, 2, 3] }
p t_rassoc { ["a", "b"] }

def t_pop_n = yield.pop(1)
p t_pop_n { [1, 2, 3] }
p t_pop_n { ["a", "b"] }

def t_shift_n = yield.shift(1)
p t_shift_n { [1, 2, 3] }
p t_shift_n { ["a", "b"] }
