# spinel: gc-stress
# An Array of objects and an Array of Strings grow out of line and read
# their length once per push, as the Integer, Float and mixed ones do: each
# element lands after the last through every path -- a full buffer, slots a
# shift left at the front taken back with room to spare or grown as well,
# a String Array still in its inline slots, a frozen Array -- and a filter's
# result collects the same elements.

class Pt
  attr_reader :v
  def initialize(v) = @v = v
end

def total(a)
  t = 0
  a.each { |o| t += o.v }
  t
end

def filled(n, base)
  a = []
  i = 0
  while i < n
    a << Pt.new(base + i)
    i += 1
  end
  a
end

objs = filled(100, 0)
p objs.length, objs[0].v, objs[99].v, total(objs)

q = filled(16, 0)
12.times { q.shift }
3.times { |k| q << Pt.new(200 + k) }
p q.length, q[0].v, q[q.length - 1].v
q2 = filled(16, 0)
2.times { q2.shift }
30.times { |k| q2 << Pt.new(300 + k) }
p q2.length, q2[0].v, q2[q2.length - 1].v, total(q2)

strs = []
40.times { |k| strs << "s#{k}" }
p strs.length, strs[0], strs[3], strs[4], strs[39]

sq = %w[a b c d e f g h]
6.times { sq.shift }
3.times { |k| sq << "t#{k}" }
p sq
sq2 = %w[a b c d]
sq2.shift
20.times { |k| sq2 << "u#{k}" }
p sq2.length, sq2.first, sq2.last

sel = objs.select { |o| o.v % 7 == 0 }
p sel.length, sel[0].v, sel[sel.length - 1].v, total(sel)
p strs.select { |s| s.end_with?("3") }

fo = [Pt.new(1)].freeze
begin
  fo << Pt.new(2)
rescue FrozenError
  puts "FrozenError #{fo.length}"
end
fs = ["a"].freeze
begin
  fs << "b"
rescue FrozenError
  puts "FrozenError #{fs.length}"
end
