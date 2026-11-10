# spinel: gc-stress
# spinel: decisions
# A loop proved over its index -- `while i < a.size`, `n.times`, `each_index`,
# an Integer Range's `each`, `0.upto`, a zip's walk -- reads every other
# array at that index in place when one test ahead of the loop says each
# reaches as far as the loop runs, with no nil below; else it runs as it
# always did. Both answer the same: a second array longer, as long, shorter
# (TypeError / nil as before), holding a nil, shifted, the same array, an
# ivar, a row an outer loop fetches, and loops that leave the test aside
# (an element write, a reassigned index). And what may not be read in place:
# a caller's same-named local in a block a method's proved loop yields to, a
# zip whose block rebinds its receiver, a row a nested block binds (stale
# after an exception leaves that block).

def w_dot(a, b)
  s = 0.0
  i = 0
  while i < a.size
    s += a[i] * b[i]
    i += 1
  end
  s
end

def w_int(a, b)
  s = 0
  i = 0
  while i < a.length
    s += a[i] - b[i]
    i += 1
  end
  s
end

def t_sum(a, b, n)
  s = 0
  n.times { |i| s += a[i] + b[i] }
  s
end

def ei_sum(a, b)
  s = 0
  a.each_index { |i| s += a[i] * b[i] }
  s
end

def r_sum(a, b, n)
  s = 0.0
  (0...n).each { |i| s += a[i] - b[i] }
  s
end

def r_incl(a, b, n)
  s = 0.0
  (0..n).each { |i| s += a[i] * b[i] }
  s
end

def u_sum(a, b, n)
  s = 0
  0.upto(n - 1) { |i| s += a[i] * 2 + b[i] }
  s
end

def zip_dot(a, b)
  s = 0.0
  a.zip(b) { |x, y| s += x * y }
  s
end

def zip_int(a, b)
  s = 0
  a.zip(b) { |x, y| s += x * y - y }
  s
end

def rows(a, t)
  s = 0.0
  t.each do |row|
    k = 0
    while k < a.size
      s += a[k] * row[k]
      k += 1
    end
    a.zip(row) { |x, y| s -= x * y * 0.5 }
  end
  s
end

def early(a, b)
  s = 0.0
  i = 0
  while i < a.size
    return s if b[i] > 3.0
    s += a[i] * b[i]
    i += 1
  end
  s
end

def zip_next(a, b)
  s = 0
  a.zip(b) do |x, y|
    next if x.odd?
    break if y > 30
    s += x + y
  end
  s
end

# nil reaches the second array: below the loop's end, or past it
def w_nil(a, b)
  s = 0.0
  i = 0
  while i < a.size
    s += a[i] * (b[i] || 100.0)
    i += 1
  end
  s
end

def zip_nil(a, b)
  out = 0.0
  a.zip(b) { |x, y| out += x * (y || -1.0) }
  out
end

def zip_far(a, b)
  out = 0.0
  a.zip(b) { |x, y| out += x * (y || -1.0) }
  out
end

# the body stores an element, or reassigns the index: no test is taken
def w_store(a, b, c)
  i = 0
  while i < a.size
    c[i] = a[i] + b[i]
    i += 1
  end
  c
end

def t_reindex(a, b, n)
  s = 0
  n.times do |i|
    i = 0 if i > 2
    s += a[i] + b[i]
  end
  s
end

# a block parameter, or a block-local, named as the loop's index is another
# variable: its reads are no reads at the proved index
def shadow(a, b, row)
  s = 0.0
  i = 0
  while i < a.size
    s += a[i] * b[i]
    row.each { |i| s += 1.5 * b[i] }
    row.each { |x; i| i = x; s -= 0.5 * b[i] }
    i += 1
  end
  s
end

class Grid
  def initialize(a, b) = (@a = a; @b = b)
  def dot
    s = 0.0
    i = 0
    while i < @a.size
      s += @a[i] * @b[i]
      i += 1
    end
    s
  end
end

fa = Array.new(20) { |i| i * 0.5 }
fb = Array.new(20) { |i| i * 0.25 }
ia = Array.new(20) { |i| i * 3 }
ib = Array.new(20) { |i| i + 7 }

p w_dot(fa, fb), w_dot(fa, fa), w_dot(fa, fb + [1.0]), w_dot([], fb)
p w_int(ia, ib), w_int(ia, ia + [5]), t_sum(ia, ib, 20), t_sum(ia, ib, 0), t_sum(ia, ib, -3)
p ei_sum(ia, ib), ei_sum(ia, ia), r_sum(fa, fb, 20), r_incl(fa, fb, 19), u_sum(ia, ib, 20)
p zip_dot(fa, fb), zip_dot(fa, fa), zip_int(ia, ib), zip_int(ia, ib + [1, 2])
p rows(fa, [fb, fa, fb.reverse]), early(fa, fb), early(fa, [0.5] * 20), zip_next(ia, ib)
p Grid.new(fa, fb).dot, Grid.new(fa, fa).dot

# shorter second arrays: as before (a zip's typed walk is left out: it
# pads with 0 where CRuby passes nil, before and after this)
[-> { w_dot(fa, fb[0, 5]) }, -> { w_int(ia, ib[0, 5]) }, -> { t_sum(ia, ib[0, 5], 20) },
 -> { ei_sum(ia, ib[0, 2]) }, -> { r_sum(fa, [1.0], 3) }, -> { u_sum(ia, ib[0, 1], 4) },
 -> { rows(fa, [fb, fb[0, 3]]) },
 -> { Grid.new(fa, fb[0, 4]).dot }].each do |f|
  p f.call
rescue => e
  p e.class
end

# shifted arrays: their elements start past the buffer's first slot
sa = ia.dup; sa.shift(3)
sb = ib.dup; sb.shift(3)
p w_int(sa, sb), t_sum(sa, sb, 17), zip_int(sa, sb), ei_sum(sa, sb)

# a nil in the second array: within the loop's end, and past it (arrays of
# their own: one that can hold nil reads every element with its nil test)
na = Array.new(20) { |i| i * 0.5 }
nb = Array.new(20) { |i| i * 0.25 }; nb[3] = nil
far = Array.new(20) { |i| i * 0.25 }; far[25] = 2.0
p w_nil(na, nb), w_nil(na, far), zip_nil(na, nb), zip_far(na, far)

# not versioned: the result is the same
p w_store(fa, fb, Array.new(20, 0.0)).sum, t_reindex(ia, ib, 8)

p shadow(fa, fb, [1, 2, 19])
begin; p shadow(fa, fb, [100000, 7]); rescue => e; p e.class; end

# frozen arrays read the same
p w_dot(fa.dup.freeze, fb.dup.freeze), zip_int(ia.dup.freeze, ib.dup.freeze)

# many passes, a collection between them
big_a = Array.new(3000) { |i| (i % 13) * 0.5 }
big_b = Array.new(3000) { |i| (i % 7) * 0.25 }
acc = 0.0
50.times do |k|
  acc += w_dot(big_a, big_b) + zip_dot(big_a, big_b)
  acc += t_sum(Array.new(100) { |i| i + k }, Array.new(100) { |i| i * k }, 100)
  Array.new(200) { |i| "s#{i}" } if k % 10 == 0
end
p acc

# a method's proved loop yields to a block whose `b[i]` / `a[i]` are the
# caller's own variables, named as the method's: no read at its index
def rep(a)
  i = 0
  while i < a.size
    yield
    i += 1
  end
end
def caller_b(a, b, i) = (s = 0; rep(a) { s += b[i] }; s)
def caller_a(a, i) = (s = 0; rep([1, 2]) { s += a[i] }; s)
[-> { caller_b([1, 2], [10, 20], 1) }, -> { caller_b([1, 2], [10, 20], 50_000) },
 -> { caller_a([10, 20], 1) }, -> { caller_a([10, 20], 50_000) }].each do |f|
  p f.call
rescue => e
  p e.class
end

# a walk whose block rebinds the receiver: the loop goes on over the new one
class Rebind
  def initialize(a, c) = (@a = a; @c = c)
  def zr(b) = (s = 0; @a.zip(b) { |x, y| @a = @c; s += y }; s)
end
def zip_rebind(a, b, c) = (s = 0; a.zip(b) { |x, y| a = c; s += y }; s)
def ei_rebind(a, b, c) = (s = 0; a.each_index { |i| a = c; s += b[i] }; s)
p zip_rebind([1, 2], [10, 20], Array.new(3000, 1)), Rebind.new([1, 2], Array.new(3000, 1)).zr([10, 20])
p ei_rebind([1, 2], [10, 20], Array.new(3000, 1))

# a row a nested block binds, left stale by an exception out of that block
def stale_rows(m, d, n)
  s = 0
  begin
    n.times { |i| m.each { |row| s += row[i] / d } }
  rescue ZeroDivisionError
    d = 1; m = [[1, 2, 3], [40, 50, 60]]; retry
  end
  s
end
def stale_rows_loop(m1, m2, d1, d2, n)
  s = 0
  k = 0
  while k < 2
    m = k == 0 ? m1 : m2
    d = k == 0 ? d1 : d2
    begin
      n.times { |i| m.each { |row| s += row[i] / d } }
    rescue ZeroDivisionError
      s += 1000
    end
    k += 1
  end
  s
end
p stale_rows([[7, 7, 7]], 0, 3), stale_rows_loop([[7, 7, 7]], [[1, 2, 3], [40, 50, 60]], 0, 1, 3)
