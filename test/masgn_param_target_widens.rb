# A parameter named among a multiple assignment's targets takes the values
# it is given there, as a plain `x = v` in the body does: an Integer or Float
# parameter assigned nil, a String or a Symbol this way holds them

# a literal right side
def tup(x, zn = nil)
  x = 7
  mk, x = 0, zn
  p [mk, x]
end
tup(1)
tup(1, "s")

def flt(x, z)
  a, x = 1, z
  p [a, x]
end
flt(1.5, nil)
flt(2.5, 3.5)

# a target after a splat
def rights(x, v)
  a, *b, x = 1, 2, v
  p [a, b, x]
end
rights(1, nil)
rights(1, :s)

# a nested target
def nest(x)
  (a, x), b = [1, nil], 2
  p [a, x, b]
end
nest(3)

# the splat target itself
def rest(r, v)
  a, *r = 1, v, v
  p [a, r]
end
rest(9, "q")

# an Array, and the pair a method answers
def arr(x, ys)
  a, x = ys
  p [a, x]
end
arr(1, ["p", "q"])

def two(k) = k ? [1, "s"] : [2, nil]
def poly(x, k)
  a, x = two(k)
  p [a, x]
end
poly(3, true)
poly(3, false)

# too few values, and one value: the rest land nil
def under(x)
  a, x = [5]
  p [a, x]
end
under(3)

def scal(x)
  a, x = 4
  p [a, x]
end
scal(3)

# under a rescue
def rz(x, z, zn = nil)
  raise "nil" if x.nil?
  mk, x = 0, zn if z.nil?
  p x
  p(x > 0)
rescue => e
  puts e.class
end
rz(1, 5)
rz(1, nil)
rz(nil, 5)
