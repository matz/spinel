# `x.send(:m)` reaches a top-level `def m` (Object's private method) only
# when x's class does not define m first. A boxed receiver, and a builtin
# one whose class defines the name, were sent to the top-level def instead.

def k(**h) = :top
class A; def k(**h) = :a; end
class B; def k(**h) = :b; end

o = [A.new, B.new][ARGV.size]
p o.send(:k, **{x: 1})
p o.send(:k, x: 1)
p o.send(:k)
p o.__send__(:k, x: 1)
p [A.new, B.new][1].send(:k)

# a class without the name reaches the top-level def, boxed or not
class C; end
p [A.new, C.new][1].send(:k)
p C.new.send(:k)
p [A.new, 5][1].send(:k)
p 5.send(:k)

# a builtin's own method comes before the top-level def
def abs = :top
def upcase = :top
p 5.send(:abs)
p (-7).send(:abs)
p "a".send(:upcase)

# and so do the methods of the modules it includes ahead of Object
def between?(a, b) = :top
p 5.send(:between?, 1, 9)
p 2.5.send(:between?, 1, 9)
p "b".send(:between?, "a", "c")

# the arguments run once, whichever method answers
$l = []
def lg(x) = ($l << x; x)
def m(v) = [:top, v]
class A; def m(v) = [:a, v]; end
p [A.new, 1][0].send(:m, lg(1))
p [A.new, 1][1].send(:m, lg(2))
p $l

# Reserve the receiver before 64 side-effecting arguments fill the override
# slots. Both sides of that boundary, and both dispatch arms, run it once.
def many(*args) = [:top, args.size]
class A; def many(*args) = [:a, args.size]; end
def logged_receiver(i)
  $l << 0
  [A.new, 1][i]
end
[0, 1].each do |i|
  $l = []
  p logged_receiver(i).send(:many,
    lg(1), lg(2), lg(3), lg(4), lg(5), lg(6), lg(7), lg(8),
    lg(9), lg(10), lg(11), lg(12), lg(13), lg(14), lg(15), lg(16),
    lg(17), lg(18), lg(19), lg(20), lg(21), lg(22), lg(23), lg(24),
    lg(25), lg(26), lg(27), lg(28), lg(29), lg(30), lg(31), lg(32),
    lg(33), lg(34), lg(35), lg(36), lg(37), lg(38), lg(39), lg(40),
    lg(41), lg(42), lg(43), lg(44), lg(45), lg(46), lg(47), lg(48),
    lg(49), lg(50), lg(51), lg(52), lg(53), lg(54), lg(55), lg(56),
    lg(57), lg(58), lg(59), lg(60), lg(61), lg(62), lg(63))
  p $l
end
[0, 1].each do |i|
  $l = []
  p logged_receiver(i).send(:many,
    lg(1), lg(2), lg(3), lg(4), lg(5), lg(6), lg(7), lg(8),
    lg(9), lg(10), lg(11), lg(12), lg(13), lg(14), lg(15), lg(16),
    lg(17), lg(18), lg(19), lg(20), lg(21), lg(22), lg(23), lg(24),
    lg(25), lg(26), lg(27), lg(28), lg(29), lg(30), lg(31), lg(32),
    lg(33), lg(34), lg(35), lg(36), lg(37), lg(38), lg(39), lg(40),
    lg(41), lg(42), lg(43), lg(44), lg(45), lg(46), lg(47), lg(48),
    lg(49), lg(50), lg(51), lg(52), lg(53), lg(54), lg(55), lg(56),
    lg(57), lg(58), lg(59), lg(60), lg(61), lg(62), lg(63), lg(64))
  p $l
end
