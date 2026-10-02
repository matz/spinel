# Read-only captures, Array elements and a direct append keep their behaviour.
def y1(v) = yield(v)
u4 = +"d"
y1(u4) { |q| g = -> { q + "x" }; p g.call }
p u4
u5 = +"e"
[u5].each { |q| h = -> { q << "%" }; h.call }
p u5
u6 = +"f"
y1(u6) { |q| q << "&" }
p u6

# Splat positions with no required parameter have no capture to inspect.
def splat_read
  s = +"s"
  yield(*[s])
end
splat_read { |q| f = -> { q + "x" }; p f.call }
def rest_read(*a) = yield(*a)
s = +"s"
rest_read(1, s) { |q| f = -> { q + 1 }; p f.call }
p s
