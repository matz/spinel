# a splat with positionals after it, into optionals beside keyword parameters,
# directly and through an instance method's dispatch
def try
  puts yield
rescue ArgumentError => e
  puts "AE #{e.message}"
end
def kw(a, b = 0, k: 1) = "kw(#{a.inspect},#{b.inspect},#{k})"
def rk(a, b = 0, k:) = "rk(#{a},#{b},#{k})"
def ti(a, b = 5) = a * 10 + b
h = {k: 7}
try { kw(*[1], 2, k: 3) }
try { kw(*[], 2, k: 3) }
try { kw(*[1, 2], 3, k: 4) }
try { kw(*[1], 2, **h) }
try { kw(*[1], 2) }
try { rk(*[1], 2, k: 3) }
try { rk(*[], 2, k: 3) }
try { ti(*[1], 2) }
try { ti(*[], 3) }
class O
  def m(a, b = 0) = "m(#{a},#{b})"
  def kw(a, b = 0, k: 1) = "kw(#{a},#{b},#{k})"
  def ti(a, b = 5) = a * 10 + b
end
o = O.new
try { o.m(*:y, 2) }
try { o.m(*[], 2) }
try { o.m(*[1, 2], 3) }
try { o.kw(*[1], 2, k: 3) }
try { o.kw(*[], 2, k: 3) }
try { o.ti(*[1], 2) }
class P
  def m(a, b = 0) = "P.m(#{a},#{b})"
end
[O.new, P.new].each do |q|
  try { q.m(*[1], 2) }
  try { q.m(*[], 2) }
end
