# A Struct or Data class written to a constant path takes the path as its
# name, as one written to a plain constant does.
module Ns; end
module A; module B; end; end

Ns::S = Struct.new(:a)
p Ns::S.new(1)
p Ns::S.name
p Ns::S
s = Ns::S.new(2)
p s.class, s.class.name
puts "#{s}"

A::B::P = Struct.new(:a)
p A::B::P.new([2]), A::B::P.to_s

::Top = Struct.new(:t)
p Top.new(1), Top.name

Ns::KW = Struct.new(:a, keyword_init: true)
p Ns::KW.new(a: 1), Ns::KW

Ns::D = Data.define(:x, :y)
d = Ns::D.new(1, 2)
p d, d.with(y: 3), d.class.name
p [Ns::S.new(Ns::S.new(1)), { k: d }]

# with a block, the class is named when the write runs, after the block
Ns::K = Struct.new(:c) do
  def hi = c
end
k = Ns::K.new(4)
p k, k.hi, k.class

# a Struct a local holds, or none, keeps no name
anon = Struct.new(:a, keyword_init: true)
p anon.new(a: 1), anon.name
puts anon.inspect.sub(/0x\h+/, "X")
