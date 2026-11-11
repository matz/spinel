# A Class value known only at run time builds a Struct or Data class it
# holds with `k[..]`, as that class's `new` does: the generated
# constructor (members by position, a plain Struct's missing ones nil,
# CRuby's ArgumentError for too many), the class's own initialize, a
# subclass's methods; beside the Array subclass, Array and Hash it may hold.
class MyA < Array
  def tag = :mya
end

S = Struct.new(:name, :n)
Pt = Struct.new(:x, :y) do
  def initialize(*args)
    super
    self.y ||= 9
  end
end
class Pt3 < Struct.new(:x, :y, :z)
  def norm = x * x + y * y + z * z
end
D = Data.define(:x, :y)
class DSum < Data.define(:a, :b)
  def sum = a + b
end

def pick(i)
  case i
  when 0 then S
  when 1 then MyA
  when 2 then Array
  when 3 then Hash
  when 4 then Pt
  when 5 then Pt3
  when 6 then D
  else DSum
  end
end

k = pick(ARGV.size)
v = k["x", 2]
p v, v.class, v.name.upcase, v.n + 1
p k["y"]
begin
  k[1, 2, 3]
rescue ArgumentError => e
  puts "ArgumentError #{e.message}"
end
w = S.new("z", 3.5)
p w

m = pick(ARGV.size + 1)[1, 2]
p m, m.class, m.tag
p pick(ARGV.size + 2)[1, 2], pick(ARGV.size + 3)[1, 2]

q = pick(ARGV.size + 4)
p q[1], q[1, 2]
p pick(ARGV.size + 5)[1, 2, 3].norm

d = pick(ARGV.size + 6)
p d[1, 2], d[1, 2].y
begin
  d[1]
rescue ArgumentError => e
  puts "ArgumentError #{e.message}"
end
begin
  d[1, 2, 3]
rescue ArgumentError => e
  puts "ArgumentError #{e.message}"
end
p pick(ARGV.size + 7)[3, 4].sum
