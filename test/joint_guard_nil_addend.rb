# A right-hand side that may be nil, read into a local once per row: the
# joint guard tests it for nil with the indices. A nil takes the updates as
# written: the ones ahead of it have stored when the TypeError is raised.
def fill(h, vals, n)
  j = 0
  while j < n
    v = vals[j]
    k = 0
    while k < 2
      hb = 2 * k
      h[hb] += 1.0
      h[hb + 1] += v
      k += 1
    end
    j += 1
  end
end
vals = [0.5, 0.25]
h = Array.new(4, 0.0)
fill(h, vals, 2)
p h
vals[3] = 1.0          # vals[2] is a gap: nil
begin
  fill(h, vals, 3)
rescue TypeError => e
  p [e.class, e.message]
end
p h

# The nil one is the first of the run: nothing is stored.
def first(h, vals, n)
  j = 0
  while j < n
    v = vals[j]
    k = 0
    while k < 2
      h[k] += v
      h[k + 1] += 1.0
      k += 1
    end
    j += 1
  end
end
g = Array.new(3, 0.0)
ws = [2.0]
first(g, ws, 1)
p g
ws[2] = 1.0
begin
  first(g, ws, 3)
rescue TypeError => e
  p [e.class, e.message]
end
p g

# An Integer array with a nilable Integer.
def ints(a, adds, n)
  j = 0
  while j < n
    add = adds[j]
    k = 0
    while k < 2
      a[k] += 1
      a[k + 1] += add
      k += 1
    end
    j += 1
  end
end
x = [0, 0, 0]
is = [3]
ints(x, is, 1)
p x
is[2] = 4
begin
  ints(x, is, 3)
rescue TypeError => e
  p [e.class, e.message]
end
p x

# A Float array with a nilable Integer.
def mixed(f, adds, n)
  j = 0
  while j < n
    add = adds[j]
    k = 0
    while k < 2
      f[k] += 0.5
      f[k + 1] += add
      k += 1
    end
    j += 1
  end
end
y = [0.0, 0.0, 0.0]
ms = [3]
mixed(y, ms, 1)
p y
ms[2] = 4
begin
  mixed(y, ms, 3)
rescue TypeError => e
  p [e.class, e.message]
end
p y
