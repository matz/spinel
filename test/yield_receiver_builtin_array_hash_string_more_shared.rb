# spinel: gc-stress
# A further layer of kilau's K-029/K-030/K-033 family: clear/compact/to_a/
# entries/fetch_values on a Hash site beside Array ones, clear on a String
# site too, several Array-only names (concat/deconstruct/difference/
# intersection/to_ary/union) never gated at all, succ's "next" alias, and
# the one-argument forms flatten(n)/max(n)/min(n)/insert(n) (kilau K-034).
#
# clear: K-033's own fix added a Hash site to clear/flatten/first/shift,
# but never considered that String also answers #clear -- a site mix
# including a String now fails the same way a Hash-less mix did before
# K-033 (kilau K-034).
#
# compact/to_a/entries/fetch_values: each has its own concrete Hash row in
# builtin_ops.c (BOPE_HASH_COMPACT, BOPE_HASH_TO_A, ...), but was never
# gated for a Hash receiver here -- same shape as clear/flatten's own
# K-033 fix, just not yet applied to these four names.
#
# concat/deconstruct/difference/intersection/to_ary/union: never gated at
# all, so even two plain Array sites of differing element kind failed to
# compile -- the same "nobody tried the base case" gap minmax turned out
# to have in K-033.
#
# next: an alias of succ (Integer/String/Symbol), whose 0-arg form was
# fixed in K-024/#9419 including a Symbol arm -- the alias name itself was
# never added to the same check.
#
# flatten(n)/max(n)/min(n)/insert(n): the one-argument forms of names
# whose 0-arg forms are already answered; the argument form was never
# added to the generic bop_rows fallback allowlist. A Hash site among
# max(n)/min(n) needed its own extra arm (the fallback allowlist alone
# only ever resolves a plain-Array site), mirroring the 0-arg redirect
# through TY_POLY_ARRAY's own row.
def t_clear = yield.clear
p t_clear { +"abc" }
p t_clear { [3, 1, 2] }
p t_clear { ["b", "a"] }
p t_clear { { a: 1 } }

def t_compact = yield.compact
p t_compact { [3, 1, 2] }
p t_compact { ["b", "a"] }
p t_compact { { a: 1 } }

def t_to_a = yield.to_a
p t_to_a { [3, 1, 2] }
p t_to_a { ["b", "a"] }
p t_to_a { { a: 1 } }

def t_entries = yield.entries
p t_entries { [3, 1, 2] }
p t_entries { ["b", "a"] }
p t_entries { { a: 1 } }

def t_fetch_values = yield.fetch_values
p t_fetch_values { [3, 1, 2] }
p t_fetch_values { ["b", "a"] }
p t_fetch_values { { a: 1 } }

def t_concat = yield.concat
p t_concat { +"abc" }
p t_concat { [3, 1, 2] }
p t_concat { ["b", "a"] }

def t_deconstruct = yield.deconstruct
p t_deconstruct { [3, 1, 2] }
p t_deconstruct { ["b", "a"] }

def t_difference = yield.difference
p t_difference { [3, 1, 2] }
p t_difference { ["b", "a"] }

def t_intersection = yield.intersection
p t_intersection { [3, 1, 2] }
p t_intersection { ["b", "a"] }

def t_to_ary = yield.to_ary
p t_to_ary { [3, 1, 2] }
p t_to_ary { ["b", "a"] }

def t_union = yield.union
p t_union { [3, 1, 2] }
p t_union { ["b", "a"] }

def t_next = yield.next
p t_next { 7 }
p t_next { "abc" }
p t_next { :ab }

def t_flatten_n = yield.flatten(2)
p t_flatten_n { [3, 1, 2] }
p t_flatten_n { ["b", "a"] }
p t_flatten_n { { a: 1 } }

def t_max_n = yield.max(2)
p t_max_n { [3, 1, 2] }
p t_max_n { ["b", "a"] }
p t_max_n { { a: 1 } }

def t_min_n = yield.min(2)
p t_min_n { [3, 1, 2] }
p t_min_n { ["b", "a"] }
p t_min_n { { a: 1 } }

def t_insert_n = yield.insert(2)
p t_insert_n { [3, 1, 2] }
p t_insert_n { ["b", "a"] }
