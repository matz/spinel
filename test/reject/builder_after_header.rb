# A header ahead of its builder in the same body. The header makes its own
# class, and the write then puts the built class in its place, so the
# header's methods are not the built class's. spinel would merge the two,
# so it refuses.
module Outer
  class Foo; def k = 2; end
  Foo = Class.new { def b = 1 }
end
p((Outer::Foo.new.k rescue :nok))
