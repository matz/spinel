# An Integer or Float Array's each, each_with_index and each_index (and the
# Enumerable methods written over each, through their yield) keep the headers
# of the arrays they index across iterations, as a while loop does: the
# receiver's own length and elements too. Each case below is one where a stale
# header would read or write the wrong memory, or where a check the cache
# stands in for has to still fire.
# spinel: gc-minor

# reads and writes of other arrays, by each, each_with_index and each_index
def axpy(g, h, w)
  i = 0
  h.each { |x| g[i] += x * w; i += 1 }
  g
end
def axpy_ewi(g, h, w)
  h.each_with_index { |x, i| g[i] += x * w }
  g
end
def axpy_ei(g, h, w) = g.each_index { |i| g[i] += h[i] * w }
p axpy([1.0, 2.0, 3.0], [0.5, 0.25, 0.125], 2.0)
p axpy_ewi([1.0, 2.0, 3.0], [0.5, 0.25, 0.125], 2.0)
p axpy_ei([1.0, 2.0, 3.0], [0.5, 0.25, 0.125], 2.0)

# writing the receiver itself, in place and past its end: the walk goes on
# over what the write added, as CRuby's does
a = [1, 2, 3]
i = 0
a.each { |x| a[i] = x * 10; i += 1 }
p a
b = [1, 2, 3]
i = 0
b.each { |x| b[i + 3] = x if b.size < 7; i += 1 }
p b
f = [1.5, 2.5]
f.each_with_index { |x, j| f[j + 2] = x * 2 if f.size < 4 }
p f

# a frozen receiver's write still raises, a nil receiver still raises
fr = [1, 2, 3].freeze
p((fr.each_with_index { |x, j| fr[j] = x } rescue $!.class))
def walk(xs) = xs.each { |x| x }
p((walk([nil, [1]][0]) rescue $!.class))

# elements that are nil: below the nil-free length the cache, past it the
# accessor (and the nil)
s = 0
[1, nil, 3].each { |x| s += x.to_i }
p s
t = 0.0
[1.5, nil, 2.5].each { |x| t += x.to_f }
p t

# break, next, return
p [1, 2, 3].each { |x| break x * 100 if x > 1 }
s = 0
[1, 2, 3].each { |x| next if x == 2; s += x }
p s
def first_big(xs) = (xs.each { |x| return x if x > 1 }; :none)
p first_big([0, 5, 9]), first_big([0])

# the Enumerable methods through their yield: count, select, inject, index
v = [0.25, 0.75, 0.5, 1.0]
w = [1.0, 2.0, 3.0, 4.0]
p v.count { |x| x > 0.5 }, v.select { |x| x >= 0.5 }, v.inject { |acc, x| acc + x }, v.index { |x| x > 0.7 }
k = 0
p v.count { |x| w[k] += x; k += 1; x > 0.5 }, w

# the same loops where their value is an interpolation's: a macro's argument
# in C, which sees no directive
u = [0.0, 0.0]
puts "at #{[4, 5, 6].find_index { |x| x > 4 }}, #{v.count { |x| x > 0.5 }} big, #{u.each_index { |j| u[j] = j * 1.5 }}"
r = 0
[3, 1].cycle(2) { |x| r += x }
puts "cycled #{r}"

# nested: an each inside a while, a while inside an each, each inside each
class Grid
  def initialize = (@v = [1.5, 2.5, 3.5]; @w = [0.0, 0.0, 0.0])
  def mirror = (i = 0; @v.each { |x| @w[i] = x * 2; i += 1 }; @w)
  def outer = (s = 0.0; @v.each { |x| @w.each { |y| s += x * y } }; s)
  def rows
    s = 0.0
    r = 0
    while r < 2
      @v.each { |x| s += x * r }
      r += 1
    end
    s
  end
  def inner_while
    s = 0
    @v.each_index do |i|
      j = 0
      while j < i
        s += j
        j += 1
      end
    end
    s
  end
end
g = Grid.new
p g.mirror, g.outer, g.rows, g.inner_while
