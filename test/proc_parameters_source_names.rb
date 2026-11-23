# `Proc#parameters` reports the names the PROGRAM wrote. Three things leaked
# the compiler's own names instead (#4045).
#
# A block parameter colliding with an outer local has a slot of its own, and
# the name the program wrote is what Proc#parameters reports -- for the
# positional kinds only, once, so rest, keyword, keyword-rest and block
# reported the slot's spelling.
#
# Reporting the written name alone was not enough either: it appears nowhere else
# in the program, so it is not in the generated symbol table, and interning it
# at emit time came too late -- the symbol rendered EMPTY. The parameter list
# interns the written name now, while the table is still open.
#
# And the implicit rest a trailing comma synthesizes (`|a,|`) is not a
# parameter Ruby reports at all.
p(->((a, b)) {}.parameters)
p(proc { |a, *b| }.parameters)
p(->(a, b = 1, *c, d:, e: 2, **f, &g) {}.parameters)
p(proc { |a, b = 1, *c, d:, e: 2, **f, &g| }.parameters)
a = 1; b = 2; c = 3; d = 4; e = 5; f = 6; g = 7
p(proc { |a, b = 1, *c, d:, e: 2, **f, &g| }.parameters)
p([a, b, c, d, e, f, g])
p(lambda { |x, (y, z)| }.parameters)
p(proc { |*| }.parameters)
p(proc { |a, | }.parameters)
p(proc { }.parameters)
