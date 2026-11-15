# map!.with_index stores each block value into the receiver at once, as
# CRuby does: a later run of the block that reads the receiver sees the
# new elements, a break keeps the ones written, and a frozen receiver
# raises before the block runs.
a = [1, 2]
a.map!.with_index { |x, i| i == 0 ? 9 : a[0] }
p a
b = [1, 2, 3]
b.map!.with_index(1) { |x, i| x + b.sum }
p b
f = [1.5, 2.5]
f.map!.with_index { |x, i| i == 0 ? 0.5 : f[0] + x }
p f
s = ["a", "b", "c"]
s.map!.with_index { |x, i| i == 0 ? "z" : s[i - 1] + x }
p s
n = [1, 2, 3]
r = n.map!.with_index { |x, i| break :stop if i == 2; x * 10 }
p r, n
m = [1, nil, 3]
m.map!.with_index { |x, i| i == 1 ? nil : (x || 0) + 1 }
p m
pa = [1, "x", :y]
pa.map!.with_index { |x, i| i == 0 ? "first" : pa[0] }
p pa
g = [1, 2, 3]
g.collect!.with_index { |x, i| g.length + i }
p g
fr = [1, 2].freeze
runs = 0
begin
  fr.map!.with_index { |x, i| runs += 1; x }
rescue FrozenError => e
  p e.class, runs
end
p fr
