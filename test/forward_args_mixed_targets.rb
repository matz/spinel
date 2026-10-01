# `...` forwarded to targets of different shapes: one taking a positional,
# another taking keywords. Each target is handed both channels and sorts out
# what it got. Before, a call that took keywords was handed the forwarder's
# own leading parameter in their place.

def m(a, **nil) = [:m, a]
def n(**kw) = [:n, kw]
def w(c, ...) = c ? m(...) : n(...)
p w(false, z: 3)
p w(true, 5)

# a rest beside keywords, and a fixed positional
def m2(a) = [:m2, a]
def n2(*r, **kw) = [:n2, r, kw]
def w2(c, ...) = c ? m2(...) : n2(...)
p w2(false, 1, z: 3)
p w2(true, 4)

# two leading parameters, an optional, and a keyword with a default
def m3(a, b = 2) = [:m3, a, b]
def n3(k: 0) = [:n3, k]
def w3(c, d, ...) = c ? m3(...) : n3(...)
p w3(true, :d, 1)
p w3(true, :d, 1, 7)
p w3(false, :d, k: 9)
p w3(false, :d)

# the block rides along
def m4(a, &blk) = [:m4, a, blk.call]
def n4(**kw) = [:n4, kw]
def w4(c, ...) = c ? m4(...) : n4(...)
p w4(true, 1) { :blk }
p w4(false, q: 1)

# a target still checks its own count
begin
  w(false, 1)
rescue ArgumentError => e
  p e.message
end
