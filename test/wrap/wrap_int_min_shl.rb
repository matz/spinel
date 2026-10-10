# spinel: not-cruby -- --int-overflow=wrap: a shift that lands on the sign
# bit gives -2**63 as a number; CRuby gives 2**63 (a Bignum) here.
# A literal count, a run-time count, a run-time operand, and the wrapping
# + - * that land exactly on -2**63 all print the number, never nil.
n = ARGV.size
p 1 << 63
p((n + 1) << 63)
x = 1 + n
p x << 63
p x << (63 + n)
p 2 << 62
p 1 << 63 + n
y = 1 << 63
p y
p y.nil?
p(y ? "truthy" : "falsy")
puts "#{y}"
p y.zero?
p y == -9223372036854775808
p 0x7fffffffffffffff + 1 + n
p 0x7fffffffffffffff + (1 + n)
p -9223372036854775807 - 2 + n + 1
p -4611686018427387904 * (2 + n)
m = -9223372036854775807 - (n + 1)
p m - 1 - n
p m - 1 - n == 0x7fffffffffffffff
p m * 2
p -m
p m.abs
p m.pred
p m + m
p m * (-1 - n)
p (m - 1 - n).nil?
p (m * 2).nil?
p (-m).nil?
p [1 << 63, 1 << 63].sum
p [1 << 63].min
a = Array.new(2, 0)
a[0] = 1 << 63
p a
p a[0].nil?
h = { 1 => 0 }
h[1] = 1 << 63
p h[1]
p h[1].nil?
s = nil
s = 1 << 63
p s
p s.nil?
