# spinel: gc-stress
# A `while i < a.size` proves its index in range from the `i = 0` just
# ahead of it, which it now sees inside a block whose emitter runs the
# body's statements itself -- a `map` block's statements ahead of its
# value, a block a yield splices -- so a second array read at the index is
# read in place behind one test, as the statement form's already was. Both
# answer what they did: a second array as long, longer, shorter (TypeError
# as before), holding a nil, empty rows, a start past 0, and the shapes that
# prove nothing -- an index the block also writes, a statement ahead of the
# loop that is not the start, a start that is the block's parameter.

def dots(a, t)
  a.map do |ai|
    t.map do |tj|
      s = 0.0
      k = 0
      while k < ai.size
        s += ai[k] * tj[k]
        k += 1
      end
      s
    end
  end
end

def dots_int(a, t)
  a.map { |ai| t.map { |tj| s = 0; k = 0; while k < ai.length; s += ai[k] - tj[k]; k += 1; end; s } }
end

# from 1: the read of a row's own element and the second one start past 0
def dots_from_one(a, t)
  a.map { |ai| t.map { |tj| s = 0; k = 1; while k < ai.size; s += ai[k] * tj[k]; k += 1; end; s } }
end

# the loop is the block's last statement: its value is nil
def last_while(a, t)
  out = 0
  a.each_with_index { |ai, q| k = 0; while k < ai.size; out += ai[k] * t[q][k]; k += 1; end }
  out
end

def each_rows(rows)
  rows.each { |r| yield r }
  rows
end

# the block a yield splices
def spliced(rows, w)
  s = 0
  each_rows(rows) do |r|
    k = 0
    while k < r.size
      s += r[k] * w[k]
      k += 1
    end
  end
  s
end

# the block writes the index elsewhere too
def stepped(a, t)
  a.map { |ai| t.map { |tj| s = 0; k = 0; while k < ai.size; s += ai[k] * tj[k]; k += 2 if ai[k] > 4; k += 1; end; s } }
end

# a statement between the start and the loop
def not_ahead(a, t)
  a.map { |ai| t.map { |tj| s = 0; k = 0; k -= 2; while k < ai.size; s += ai[k] * tj[k]; k += 1; end; s } }
end

# the start is the block's parameter
def param_start(a, t, ks)
  ks.map { |k| a.map { |ai| s = 0; while k < ai.size; s += ai[k] * t[0][k]; k += 1; end; s } }
end

def try
  yield
rescue TypeError, NoMethodError => e
  e.class.to_s
end

def rows(n, m, k)
  Array.new(n) { |i| Array.new(m) { |j| (i * m + j) * k } }
end

def irows(lens)
  Array.new(lens.size) { |i| Array.new(lens[i]) { |j| i * 3 + j + 1 } }
end

a = rows(4, 5, 0.5)
t = rows(3, 5, 0.25)
p dots(a, t)
p dots(a, rows(3, 6, 0.25))
p try { dots(a, rows(3, 3, 0.25)) }
tn = rows(1, 2, 1.0)
tn[0][5] = 1.0
p try { dots(a, tn) }
p dots(rows(2, 1, 1.0), rows(2, 2, 2.0))
p dots(rows(2, 0, 1.0), t)
ai = irows([6, 6, 6, 6])
ti = irows([6, 6])
p dots_int(ai, ti)
p try { dots_int(ai, irows([3, 3])) }
p dots_from_one(ai, ti)
p try { dots_from_one(ai, irows([6, 1])) }
p last_while(ai, irows([6, 6, 6, 6]))
p try { last_while(ai, irows([6, 6, 2, 6])) }
sr = Array.new(4) { |i| Array.new(6) { |j| i * 6 + j } }
sw = Array.new(6) { |j| j - 2 }
p spliced(sr, sw)
p try { spliced(sr, Array.new(4) { |j| j }) }
p stepped(ai, ti)
p try { stepped(ai, irows([2, 2])) }
p try { not_ahead(ai, ti) }
p param_start(ai, ti, [0, 2])
p try { param_start(ai, irows([3]), [0]) }
