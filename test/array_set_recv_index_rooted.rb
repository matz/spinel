# spinel: gc-minor
# spinel: gc-stress
# `recv[idx] = value` reads the receiver before the index. Where both can
# allocate, the receiver is held in a rooted temporary while the index runs,
# whatever the value is: a frozen String literal, a String that is made, an
# Integer, a Float. Each receiver here is a fresh object nothing else holds,
# and each index allocates, so a receiver left unrooted is collected under
# SPINEL_GC_STRESS while the index runs.
class Box
  def initialize
    @s = [+"x", +"y", +"z"]
    @n = [1, 2, 3]
    @f = [1.5, 2.5, 3.5]
  end
  attr_reader :s, :n, :f
end

def mk = Box.new
def f(i) = ("q" * (i + 50)).size - (i + 50) + i

keep = []
200.times do |i|
  mk.s[f(i % 3)] = "lit"
  mk.s[f(i % 3)] = "n#{i}"
  mk.n[f(i % 3)] = 5
  mk.f[f(i % 3)] = 2.5
  x = mk
  x.s[f(1)] = "lit"
  x.n[f(2)] = i
  x.f[f(0)] = i * 0.5
  keep << x
end
p keep.size
p keep.map { |b| b.s[1] }.uniq
p keep.map { |b| b.n[2] }.uniq.size
p keep.map { |b| b.f[0] }.uniq.size
p keep[7].s, keep[7].n, keep[7].f

# a value that runs statements before it answers a literal is not the
# literal alone: the receiver is rooted while those run
def g(i) = ("q" * (i + 60)).size
n = 0
100.times do |i|
  mk.s[1] = (g(i); "lit")
  mk.s.push((g(i); "pl"))
  mk.s << (g(i); "sh")
  mk.s[f(i % 3)] = (g(i); "both")
  n += 1
end
p n
