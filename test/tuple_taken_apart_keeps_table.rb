# A table a method hands out in a tuple, taken apart where it lands
# (`a, b = m`), keeps its kind: each target holds the very table the
# method built. Where the tuple goes anywhere else, the table stays boxed.
# spinel: gc-stress

# two tables built together, held in constants and read from methods
def cover_tables(n)
  cols = Array.new(n) { Array.new(2, 0) }
  i = 0
  while i < n
    cols[i][0] = i
    cols[i][1] = n + i % 2
    i += 1
  end
  sizes = Array.new(n) { Array.new(1, 0) }
  [cols, sizes]
end

COLS, SIZES = cover_tables(4)

def degree(cols, sizes, c)
  sizes[c][0] += cols[c][0] + cols[c][1]
  sizes[c][0]
end
p (0...4).map { |c| degree(COLS, SIZES, c) }

# one table under two names
def twin
  t = Array.new(2) { Array.new(2, 0) }
  [t, t]
end
tw_a, tw_b = twin
tw_a[0][0] = 5
tw_b[1] << 9
p tw_b[0][0], tw_a[1]

# `return a, b`, a Float table built in place, an Integer beside it
def grid(n)
  return Array.new(n) { |i| Array.new(n) { |j| (i + j) * 0.5 } }, n
end
gr_m, gr_n = grid(3)
gr_s = 0.0
gr_n.times { |i| gr_s += gr_m[i][i] }
p gr_s

# Arrays of objects
class Pt
  attr_reader :x

  def initialize(x)
    @x = x
  end
end

def points
  a = [Pt.new(1), Pt.new(2)]
  b = Array.new(3) { |i| Pt.new(i * 10) }
  [a, b]
end
pt_a, pt_b = points
pt_a << Pt.new(5)
p pt_a.sum(&:x) + pt_b.map(&:x).sum

# taken apart in a loop body and in an ensure clause
def pair
  t = Array.new(2) { Array.new(2, 1) }
  [t, 2]
end
lp_i = 0
while lp_i < 3
  lp_i += 1
  lp_a, lp_b = pair
end
begin
  lp_i += 1
ensure
  en_a, en_b = pair
end
p lp_a[0][0] + lp_b + en_a[1][1] + en_b + lp_i

# the tuple also kept whole: a String stored through it
def shared
  t = Array.new(2) { Array.new(2, 0) }
  [t, t]
end
sh_a, sh_b = shared
sh_whole = shared
sh_whole[0][0][0] = "s"
p sh_a[0][0] + sh_b[1][1], sh_whole[1][0][0]

# the multi-write's own value read
def kept
  t = Array.new(2) { Array.new(2, 0) }
  [t, 1]
end
kp_whole = (kp_a, kp_b = kept)
kp_whole[0][0][0] = "s"
p kp_a[1][1] + kp_b, kp_whole[0][0][0]

# the method reached by send
def sent
  t = Array.new(2) { Array.new(2, 0) }
  [t, 1]
end
se_a, se_b = sent
se_q = send(:sent)
se_q[0][0][0] = "s"
p se_a[0][0] + se_b, se_q[0][0]

# a splat target
def spread
  t = Array.new(2) { Array.new(2, 0) }
  [t, t, 3]
end
sp_a, *sp_rest = spread
sp_rest[0][1][1] = "s"
p sp_a[0][0], sp_rest[0][1], sp_rest[1]
