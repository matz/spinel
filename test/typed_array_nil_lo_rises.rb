# An Integer or Float Array whose nils are all overwritten reads like one that never held any
# The lowest slot that can hold a nil (nil_lo) only ever went down: once
# a nil had been in the array, a loop read every element at or past it
# through the checked path, after every nil was gone. It now moves up as
# the nil at it is overwritten, and a bitmap with no nil left is dropped;
# a nil stored afterwards brings a bitmap back.

def total(a)
  s = 0
  i = 0
  while i < a.size
    s += a[i]
    i += 1
  end
  s
end

x = ARGV.size > 9 ? 1 : nil
fx = ARGV.size > 9 ? 1.0 : nil

# a gap, filled in place
g = [1]
g[5] = 6
p g, g.count(nil)
i = 1
while i < 5
  g[i] = i + 1
  i += 1
end
p g, total(g), g.include?(nil)
# a nil again, after the last one was overwritten
g[2] = x
p g, g.index(nil), g.compact.sum
g[2] = 3
p total(g)

# all nil (a nullable fill), filled out of order
order = [3, 0, 7, 1, 6, 2, 5, 4]
z = Array.new(8, x)
order.each_with_index { |o, k| z[o] = k * 10 }
p z, total(z), z.count(nil)

# filled up to one slot, which stays nil
w = Array.new(4, x)
w[0] = 1
w[1] = 2
w[3] = 4
p w, w.compact.sum
begin
  total(w)
rescue TypeError => e
  p e.class
end

# shifted, then filled
sh = Array.new(5, x)
sh.shift
sh[0] = 9
sh[1] = 8
sh[2] = 7
sh[3] = 6
p sh, total(sh)
sh.insert(1, x)
p sh, sh.count(nil)
sh[1] = 5
p total(sh)

# a Float Array
fz = Array.new(4, fx)
fz[0] = 0.5
fz[3] = 2.0
fz[1] = 1.0
fz[2] = 1.5
fs = 0.0
fz.each { |v| fs += v }
p fz, fs, fz.include?(nil)
fz[1] = fx
p fz, fz.compact.sum
