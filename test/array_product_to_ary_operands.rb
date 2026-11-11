# Array#product with two or more operands converts each one through #to_ary,
# as CRuby does, and raises TypeError for an operand that is not an Array.
# An object operand read as it was had no elements, so the product was [].
class Src
  def initialize(*a); @a = a; end
  def to_ary; @a; end
end
class Bad; def to_ary = 3; end
b = [1, 2]
p b.product(Src.new(5, 6), [7])
p b.product([7], Src.new(5, 6))
p b.product(Src.new(5, 6))
r = []
p b.product(Src.new(5, 6), [7]) { |t| r << t }.equal?(b), r
x = [b, "s"].first
p x.product(Src.new(3), Src.new(4))
[nil, {a: 1}, 1..2, Object.new, Bad.new].each do |o|
  begin
    p b.product([1], o)
  rescue TypeError => e
    p [e.class, e.message.sub(/0x\h+/, "")]
  end
end
p b.product([], Src.new(1))
class MyA < Array; end
S = Struct.new(:a)
b = [1, 2]
m = MyA.new([8, 9])
p b.product(m, [7])
p b.product([7], m)
begin; p b.product([7], S.new(1)); rescue TypeError => e; p e.message; end
f = [1.5, 2.5]
p f.product(Src.new("x"), %w[a b])
s = %w[q r]
p s.product(Src.new(:k, :l), [nil])
big = (1..30).to_a
n = 0
big.product(Src.new(*(1..30).to_a), Src.new(1, 2)) { |t| n += t.sum }
p n
z = [b, 1].first
p z.product([3], Src.new(4, 5))
p b.product(Src.new(5), [7]).size
p b.product(Src.new, [7])
# A #to_ary that builds a fresh Array each time: the converted Arrays stay
# alive while the product is built
class Fresh
  def initialize(k); @k = k; end
  def to_ary; [@k, @k + 1, @k + 2]; end
end
i = 0
last = nil
while i < 2000
  last = b.product(Fresh.new(i), [7], Fresh.new(i * 2))
  raise "bad #{last.inspect}" unless last.size == 18 && last[0] == [1, i, 7, i * 2]
  i += 1
end
p last[0]
p [[1, 2], "s"].first.product(Fresh.new(5), [8]).size
# a #to_ary that answers nil is a TypeError (the conversion is strict)
class NilAry; def to_ary; nil; end; end
begin; b.product([9], NilAry.new); rescue TypeError => e; p e.message; end
class PrivAry; private def to_ary; [5, 6]; end; end
p b.product(PrivAry.new, [0])
# a later operand's to_ary changes an Array an earlier operand gave, or the
# receiver: the product reads them after every operand is converted
$shared = [1, 2]
class ShA; def to_ary = $shared; end
class ShB; def to_ary; $shared << 3; [9]; end; end
p [0].product(ShA.new, ShB.new)
seen = []
[0].product(ShA.new, ShB.new) { |t| seen << t }
p seen.size, $shared
$recv = [4, 5]
class ShR; def to_ary; $recv << 6; [7]; end; end
p $recv.product([8], ShR.new).size
