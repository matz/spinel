# spinel: gc-stress
# A zip block's argument parameter is nil past the end of a shorter
# operand, as CRuby yields it: a Float or Integer operand's missing
# element is no 0.0 or 0. So is a nil element of the operand, and an
# element of a row a transpose read past another row's end. An operand
# that is no typed Array (a Range, a Hash, a value boxed at one call)
# hands its elements over as they are.

def fdot(a, b)
  s = 0.0
  a.zip(b) { |x, y| s += x * y }
  s
end

def idot(a, b)
  s = 0
  a.zip(b) { |x, y| s += x * y }
  s
end

def try
  yield
rescue TypeError => e
  "#{e.class}: #{e.message}"
end

fa = [1.0, 2.0, 3.0]
p fdot(fa, [3.0, 4.0, 5.0])
p fdot(fa, [3.0, 4.0, 5.0, 6.0])
p try { fdot(fa, [3.0, 4.0]) }
p try { fdot(fa, []) }
p fdot([], [1.0])
ia = [1, 2, 3]
p idot(ia, [3, 4, 5])
p idot(ia, [3, 4, 5, 6])
p try { idot(ia, [3, 4]) }

# the same walk over and over, the operand shorter only at the last call
long = Array.new(50) { |i| i * 0.5 }
acc = 0.0
40.times { |k| acc += fdot(long, long) + k }
p acc
p try { fdot(long, long.first(49)) }

# the parameter's nil, tested
def fill(a, b, d)
  out = []
  a.zip(b) { |x, y| out << (y.nil? ? d : x + y) }
  out
end
p fill([1.0, 2.0, 3.0], [0.5], -1.0)
p fill([1, 2, 3], [10, 20], 0)

# a nil element below the end, a gap past it, and an operand boxed at
# one call and a Float Array at another
def zor(a, b)
  out = 0.0
  a.zip(b) { |x, y| out += x * (y || -1.0) }
  out
end
na = Array.new(8) { |i| i * 0.5 }
nb = Array.new(4) { |i| i * 0.25 }
nb[2] = nil
far = Array.new(4) { |i| i * 0.25 }
far[6] = 2.0
p zor(na, nb), zor(na, far), zor(na, [1.5, 2.5])

# rows of a transpose: a row may hold nil where another row is shorter
def matmul(a, t)
  a.map { |ai| t.map { |tj| s = 0.0; ai.zip(tj) { |x, y| s += x * y }; s } }
end
def imatmul(a, t)
  a.map { |ai| t.map { |tj| s = 0; ai.zip(tj) { |x, y| s += x * y }; s } }
end
b = Array.new(3) { |i| Array.new(3) { |j| (i + j) * 0.25 } }
t = Array.new(3) { |j| Array.new(3) { |i| b[i][j] } }
p matmul(b, t)
ragged = [[1.0, 2.0], [3.0]]
rt = Array.new(2) { |j| Array.new(2) { |i| ragged[i][j] } }
p rt
p try { matmul(Array.new(1) { [1.0, 1.0] }, rt) }
p matmul(Array.new(1) { [1.0] }, rt)
ib = [[1, 2], [3, 4, 5]]
it = Array.new(3) { |j| Array.new(2) { |i| ib[i][j] } }
p it
p try { imatmul(Array.new(1) { [1, 1] }, it) }
p imatmul(Array.new(1) { [2, 3] }, it.first(2))

# an operand that is no typed Array: its elements as it hands them over,
# nil past its end
def gather(a, r)
  out = []
  a.zip(r) { |x, y| out << y }
  out
end
p gather([1, 2, 3], 1..2), gather([1.0, 2.0], 1..5), gather([1, 2], { a: 1 })
p gather([1.0, 2.0], [2, 3]), gather([1.0, 2.0], [1.5])
def isum(r)
  s = 0
  [1, 2].zip(r) { |x, y| s += x * (y || 0) }
  s
end
p isum(1..2), isum([3, 4]), isum([5])
sh = [9, 1, 2]
sh.shift
p isum(sh), gather([1, 2, 3], [5, 6].each), gather([1, 2], "ab".each_char)
sh2 = [9, 3, 4]
sh2.shift
p idot([1, 1], sh2), try { idot(ia, sh2) }

# a block that rebinds the receiver or the operand: the walk goes on over
# the objects the call was made with, as long as the receiver is
def rebind_recv(a, b, c) = (s = 0; a.zip(b) { |x, y| a = c; s += x * y }; s)
def rebind_arg(a, b, c) = (s = 0; a.zip(b) { |x, y| b = c; s += x * y }; s)
p rebind_recv([1, 2], [10, 20], Array.new(50, 1)), rebind_arg([1, 2, 3], [10, 20, 30], [0])
p try { rebind_recv([1, 2, 3], [10, 20], Array.new(50, 1)) }
def rebind_any(a, b, c) = (s = 0; a.zip(b) { |x, y| b = c; s += y }; s)
p rebind_any([1, 2], [10, 20], [0]), rebind_any([1.0, 2.0], [10, 20], [0])
