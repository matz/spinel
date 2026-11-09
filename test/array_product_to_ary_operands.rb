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
