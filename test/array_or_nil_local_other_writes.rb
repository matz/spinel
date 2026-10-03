# A local every plain write of which stores an array or nil is indexed
# without the dispatch on what it holds. Only `x = v` was looked at, so a
# local set to nil and then filled by `||=`, by `&&=`, by a multiple
# assignment or by a `for` was taken to hold an array while it held a Hash
# or a String, and its index answered nil. The same went for a local
# holding arrays of arrays, one element of which is read into another local.
def or_write(k, v)
  h = nil
  h ||= {}
  h[k] = v
  h[1]
end
p or_write(1, "one")
p or_write(1, :sym)

def or_write_param(x)
  s = nil
  s ||= x
  s[1]
end
p or_write_param("hello")
p or_write_param([1, 2, 3])
p or_write_param({1 => "v"})

def and_write(x)
  s = []
  s &&= x
  s[0]
end
p and_write("hello")
p and_write({0 => :z})

def masgn(x)
  h = nil
  h, y = x, 1
  h[0]
end
p masgn("abc")
p masgn({0 => :z})
p masgn([7, 8])

def for_target(xs)
  e = nil
  for e in xs
  end
  e[0]
end
p for_target(["ab", "cd"])
p for_target([{0 => 1}])

# every write an array or nil: still an array, however it was written
def still_array(n)
  a = nil
  a ||= [n, n + 1]
  a[1]
end
p still_array(4)

# one element of a local that held arrays, after the local was rebound
def and_container(x)
  tbl = [[1, 2], nil]
  tbl &&= x
  e = tbl[0]
  e[0]
end
p and_container(["hello"])
p and_container([{0 => :z}])

def masgn_container(x)
  tbl = [[1, 2], nil]
  tbl, y = x, 1
  e = tbl[0]
  e[0]
end
p masgn_container(["abc"])
p masgn_container([{0 => :q}])
