# A class made at run time answers to_s, inspect and === through its own
# class methods, in an interpolation, a p, a map and a case/when.

def mk_show = Class.new { def self.to_s = "C(#{name.inspect})"; def self.inspect = "I(#{name.inspect})" }
A = mk_show
B = mk_show
puts "#{A}", "#{B}"
p A, B
puts A.to_s, B.inspect
p [A].map(&:to_s)

def mk_eqq = Class.new { def self.===(o) = (o == self) }
x = mk_eqq
y = mk_eqq
p x === x, x === y, (case x when y then :y when x then :x end)

def mk_int = Class.new { def self.===(o) = o.is_a?(Integer) }
z = mk_int
p(case 5 when z then :int else :no end, case "s" when z then :int else :no end)

def mk_member = Class.new { def hi = 1 }
m = mk_member
n = mk_member
[m.new, n.new, nil, 3].each do |v|
  p(case v when n then :n when m then :m else :none end)
end

# a constant holding such a class asks its own === too
C = mk_int
D = mk_member
p(case 7 when C then :c else :no end, case D.new when C then :c when D then :d else :no end)
