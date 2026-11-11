# The fixture of `make emit-trace-test`: each line below holds one call whose
# arm of the call chain the test names. In a directory of its own, so the
# feature suite's test/*.rb does not pick it up.
a = [1, 2, 3]
b = a.push(4)
n = b.length
m = n + 2
puts m
def twice(x)
  return x * 2
end
puts twice(m)
