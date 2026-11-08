# Struct.new and Data.define with member names from `%w[...].map(&:to_sym)`
# (or `&:intern`, or the same block written out), directly or through a
# constant: the compiler reads the names, as it does a literal symbol list
# (#11342)
FIELDS = %w[a b].map(&:to_sym)
Row = Struct.new(*FIELDS, keyword_init: true)
p Row.members
r = Row.new(**{ a: 1, b: 2 })
p r.a
p r.b
G = %w[x y].map(&:intern).freeze
P = Struct.new(*G)
p P.new(1, 2).y
Q = Struct.new(*%w[m n].map { |s| s.to_sym })
p Q.new(3, 4).to_a
D = Data.define(*%w[w h].map(&:to_sym))
d = D.new(w: 5, h: 6)
p d.h
p d
