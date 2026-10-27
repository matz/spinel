# spinel: int64
# A Set finds its members by hash and then equal? or eql?, as CRuby's does,
# both up to eight members (along its Array) and past eight (through its
# Hash index): value objects that define eql?/hash, 1 beside 1.0, the same
# NaN, eql? without hash, Strings joined as frozen copies, and the same
# answers after every method that rebuilds the members. Each check runs on a
# Set of a few members and on the same Set with twenty more. Past eight, a
# member changed after it joined is looked for under the hash it had then,
# until reset (CRuby's Set calls that state unreliable; reset is the cure).

class Pt
  attr_reader :x, :y
  def initialize(x, y); @x = x; @y = y; end
  def eql?(o); o.is_a?(Pt) && o.x == x && o.y == y; end
  def hash; [x, y].hash; end
  def inspect; "Pt(#{x},#{y})"; end
end

# eql? with no hash: CRuby's Set keeps two equal-looking instances apart
class OnlyEql
  attr_reader :v
  def initialize(v); @v = v; end
  def eql?(o); o.is_a?(OnlyEql) && o.v == v; end
end

def padded(members, pad)
  s = Set.new
  pad.times { |i| s << 1000 + i }
  members.each { |m| s << m }
  s
end

def bits(s, qs)
  qs.map { |q| s.include?(q) ? "1" : "0" }.join
end

NAN = Float::NAN
QS = [NAN, 0.0, -0.0, 1.0, 1, 2**70, "a", :a, nil, false, true, [1, 2], { k: 1 }, Pt.new(1, 2), Pt.new(2, 1), 1.5]
GROUPS = [[NAN, 0.0, 1, 2**70], ["a", :a, nil, false], [[1, 2], { k: 1 }, Pt.new(1, 2), 1.5]]

[0, 20].each do |pad|
  puts "-- pad #{pad}"
  GROUPS.each do |g|
    s = padded(g, pad)
    n0 = s.size - pad
    adds = [Pt.new(1, 2), -0.0, 2**70, 1.0, "a", [1, 2]].map { |m| s.add?(m).nil? ? "=" : "+" }.join
    p [n0, bits(s, QS), adds, s.size - pad]
    g.each { |m| s.delete(m) }
    p [s.size - pad, bits(s, QS)]
  end
  p padded([OnlyEql.new(1), OnlyEql.new(1)], pad).size - pad

  nl = padded([nil, false, 1], pad)
  nl.delete(5)
  p [nl.size - pad, nl.include?(nil), nl.delete?(7).nil?]
  nl.delete(nil)
  p [nl.size - pad, nl.include?(nil), nl.include?(false)]

  str = +"ab"
  t = padded([], pad)
  t << str
  str << "c"
  p [t.include?("ab"), t.include?("abc"), t.to_a.last.frozen?, t.size - pad]

  u = padded([3, 1, 2], pad)
  u.delete(1)
  u << 1
  u << 3
  p u.to_a.last(3)
  p [u.delete?(9).nil?, u.delete?(2).to_a.last(2), u.include?(2)]

  c = padded([1, 2, 3, 4, 5, 6], pad)
  c.select! { |x| x > 999 || x.even? }
  p [c.size - pad, c.include?(2), c.include?(1)]
  c.reject! { |x| x == 4 }
  p [c.size - pad, c.include?(4), c.include?(6)]
  c.keep_if { |x| x != 2 }
  p [c.size - pad, c.include?(2), c.include?(6)]
  c.delete_if { |x| x == 6 }
  p [c.size - pad, c.include?(6), c.include?(1000) == (pad > 0)]
  c.add(7)
  p [c.include?(7), c.size - pad]
  m = padded([1, 2, 3], pad)
  m.map! { |x| x > 999 ? x : x % 2 }
  p [m.size - pad, m.include?(1), m.include?(2), m.include?(0)]

  d = padded([1, 2], pad)
  e = d.dup
  e << 3
  d.delete(1)
  p [d.include?(1), e.include?(1), e.include?(3), d.include?(3)]
  cl = e.clone
  cl.delete(3)
  p [cl.include?(3), e.include?(3)]

  f = padded([1, 2], pad)
  f.replace(padded([5, 6], pad))
  p [f.size - pad, f.include?(1), f.include?(5)]
  f.replace([7, 7, 8])
  p [f.to_a, f.include?(5), f.include?(7)]
  f.clear
  p [f.size, f.include?(7)]
  f << 7
  p [f.size, f.include?(7)]

  g = padded([1, Set[2, Set[3]]], pad)
  g.flatten!
  p [g.size - pad, g.include?(3), g.include?(1), g.include?(Set[3])]

  h = padded([Set[1, 2], Set[3]], pad)
  p [h.include?(Set[2, 1]), h.include?(Set[3, 4]), h.add?(Set[1, 2]).nil?, h.size - pad]

  a = padded([1, 2, 3, 4], pad)
  b = Set[3, 4, 5]
  p [(a | b).size - pad, (a & b).to_a, (a - b).size - pad, a.subset?(a | b), a.superset?(b), a.disjoint?(Set[9])]
  p [a == padded([4, 3, 2, 1], pad), a.hash == padded([4, 3, 2, 1], pad).hash]
  p [a.merge([9, 1]).size - pad, a.subtract([1, 9]).size - pad, a.include?(1), a.include?(9), a.include?(2)]

  # reset finds a changed member again, whatever the size
  k = [1]
  w = padded([k, 2], pad)
  k << 9
  w.reset
  p [w.include?([1, 9]), w.include?([1]), w.size - pad]

  begin
    padded([1], pad).freeze.reset
  rescue FrozenError => ex
    p ex.class
  end
end

# past eight members, a member changed after it joined is looked for under
# the hash it had then, as CRuby's Set does, until reset
k = [1]
w = padded([k, 2], 20)
k << 9
p [w.include?([1, 9]), w.include?([1]), w.size]
w.add([1, 9])
p w.size
w.reset
p [w.include?([1, 9]), w.size]
v = padded([[1], [2]], 20)
v.to_a[20] << 3
v.delete([1, 3])
p [v.size, v.to_a.last(2)]
v.reset
v.delete([1, 3])
p [v.size, v.to_a.last(2)]
# the same Array joins twice once it has changed; delete takes out the
# entry found under its hash now, the later one
k2 = [1]
z = padded([k2, 2], 20)
k2 << 5
z << k2
p [z.size, z.to_a.last(3)]
z << 3
z.delete(k2)
p [z.size, z.to_a.last(3)]

# a filter keeps the index past eight members; clear drops it
k3 = [1]
q = padded([k3], 20)
q.select! { true }
k3 << 2
p [q.include?([1, 2]), q.include?([1]), q.size]
bc = padded([1], 20)
bc.clear
p [bc.include?(1000), bc.include?(1), bc.size]

# crossing eight members both ways keeps every answer
x = Set.new
i = 0
r = []
while i < 12
  x << i * 10
  r << (0..i).all? { |j| x.include?(j * 10) } && !x.include?(i * 10 + 1)
  i += 1
end
p r.all?
while x.size > 3
  x.delete(x.first)
end
p [x.to_a, x.include?(90), x.include?(80)]
y = Set.new(1..20)
y.select! { |n| n < 4 }
p [y.to_a, y.include?(3), y.include?(4)]
y.merge(10..19)
p [y.size, y.include?(15), y.include?(9)]

# enough members to grow the table several times
big = Set.new
i = 0
while i < 3000
  big << i * 7
  i += 1
end
hits = 0
i = 0
while i < 3000
  hits += 1 if big.include?(i * 3)
  i += 1
end
p [big.size, hits, big.include?(20993), big.include?(20994)]
big.select! { |n| n % 2 == 0 }
p [big.size, big.include?(14), big.include?(7)]
strs = Set.new
i = 0
while i < 2000
  strs << "s#{i % 1500}"
  i += 1
end
p [strs.size, strs.include?("s1499"), strs.include?("s1500")]
