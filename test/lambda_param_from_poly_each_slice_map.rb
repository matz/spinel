# The block params of each_slice(n).map / each_cons(n).map on a boxed
# receiver are the elements of its slices, typed while the analysis runs: a
# lambda they are handed to takes them as they are, not as Integers.
F = lambda { |v| v.is_a?(String) ? Float(v) : v }
P = lambda { |v| v.is_a?(Array) ? v.each_slice(2).map { |a, b| F.call(b) } : v }
p P.call(["a", "1.5"])
p P.call(7)

Q = lambda { |v| v.is_a?(Array) ? v.each_slice(2).map { |x| F.call(x[1]) } : v }
p Q.call(["b", "2.5"])
p Q.call(8)

C = lambda { |v| v.is_a?(Array) ? v.each_cons(2).map { |a, b| F.call(b) } : v }
p C.call(["a", "1.5", "2"])
p C.call(9)

def pr(v) = v.is_a?(Array) ? v.each_slice(2).map { |a, b| F.call(b) } : v
p pr(["c", "3.5"])
p pr(10)
