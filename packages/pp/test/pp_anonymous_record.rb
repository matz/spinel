# pp of a Data and a Struct whose class has no name. CRuby's
# Data#pretty_print writes "#<data" with no space after it, and its
# Struct#pretty_print writes "#<struct " with the space, so a Struct with no
# name has two spaces before its first member. The pp package wrote the
# Data with two spaces too.
require "pp"

d = Data.define(:q).new(q: 1)
print d.pretty_inspect
pp d
pp Data.define(:name, :items).new(name: "n" * 40, items: (1..12).to_a)
pp [Data.define(:a, :b).new(a: 1, b: [2, 3])]

s = Struct.new(:a).new(1)
print s.pretty_inspect
pp [Struct.new(:a, :b).new(1, [2, 3])]

Named = Data.define(:q)
pp Named.new(q: 1)
