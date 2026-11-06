# A method that passes its own block on to instance_exec, instance_eval or
# then (`def look(t, &b) = t.instance_exec(&b)`) answers that block's value.
# Where a caller's block leaves a nil in the receiver's Integer slot, the
# method kept a plain Integer return while the block's value was its oint,
# and the C did not compile. A `next nil` in an instance_exec block stored
# 0 (or did not compile) the same way, and over a boxed receiver the
# arm's nil-able value was boxed as a plain Integer. A trampoline method's
# call (`def fire(&b) = instance_exec(&b)` on self) is spliced as the
# instance_exec, and was read as the method's plain value.
class T
  attr_accessor :flag
  def initialize = (@flag = 3)
  def fire(&b) = instance_exec(&b)
  def fire_eval(&b) = instance_eval(&b)
  def fire2(n, &b) = instance_exec(n, &b)
end

class TT < T; end

class M
  def look(t, &b) = t.instance_exec(&b)
  def look_eval(t, &b) = t.instance_eval(&b)
  def anon(t, &) = t.instance_exec(&)
  def outer(t, &b) = look(t, &b)
  def with_arg(t, n, &b) = t.instance_exec(n, &b)
  def via(x, &b) = x.then(&b)
  def two(t, &b)
    t.flag = 4
    t.instance_exec(&b)
  end
end

t = T.new
m = M.new
p m.look(t) { @flag = nil }
p m.look(t) { @flag }
p m.look(t) { @flag = 5 }
p m.look(t) { 6 }
p m.look(t) { 6 } + 1
p m.look_eval(t) { @flag = nil }
p m.anon(t) { @flag }
p m.outer(t) { @flag = 7 }
p m.outer(t) { @flag = nil }
p m.with_arg(t, 2) { |a| a > 1 ? nil : a }
p m.with_arg(t, 1) { |a| a > 1 ? nil : a }
p m.via(3) { |v| v > 2 ? nil : v }
p m.via(1) { |v| v > 2 ? nil : v }
p m.two(t) { next nil if @flag == 4; 1 }
p m.two(t) { next if @flag == 4; 1 }
p m.two(t) { next 9 if @flag == 4; 1 }
p (m.look(t) { @flag } || 0) + 1
p m.look(t, &proc { @flag })
p((x = m.look(t) { @flag = 8 }; x))
p((x = m.look(t) { @flag = nil }; x.nil?))
[t, 1].each { |o| p m.look(o) { @flag } }
t.flag = nil
[t, 1].each { |o| p m.look(o) { @flag } }
[t, 1].each { |o| p o.instance_exec { @flag } }
p m.look(nil) { @flag }

# the same `next` written at the call
t.flag = 4
p t.instance_exec { next nil if @flag == 4; 1 }
p t.instance_exec { next nil if @flag == 3; 1 }
p t.instance_exec { next if @flag == 4; 1 }
p t.instance_exec { next nil if @flag == 4; 1.5 }
p t.instance_exec { next nil if @flag == 4; "s" }

# a trampoline on self, also called with a proc value and on a subclass
u = T.new
p u.fire { @flag = nil }
p u.fire { @flag }
p u.fire { 1 }
u.fire { puts 2 }
p u.fire_eval { @flag = 4 }
p u.fire_eval { @flag = nil }
p u.fire2(2) { |n| n > 1 ? nil : n }
p u.fire2(1) { |n| n > 1 ? nil : n }
pr = proc { @flag }
p u.fire(&pr)
p TT.new.fire { @flag = nil }
p TT.new.fire { 5 }

# a nil caller after more callers than the analysis lists one by one
class M
  def many(t, &b) = t.instance_exec(&b)
end
t.flag = 3
m.many(t) { 1 }; m.many(t) { 2 }; m.many(t) { 3 }; m.many(t) { 4 }; m.many(t) { 5 }; m.many(t) { 6 }; m.many(t) { 7 }; m.many(t) { 8 }; m.many(t) { 9 }; m.many(t) { 10 };
m.many(t) { 11 }; m.many(t) { 12 }; m.many(t) { 13 }; m.many(t) { 14 }; m.many(t) { 15 }; m.many(t) { 16 }; m.many(t) { 17 }; m.many(t) { 18 }; m.many(t) { 19 }; m.many(t) { 20 };
m.many(t) { 21 }; m.many(t) { 22 }; m.many(t) { 23 }; m.many(t) { 24 }; m.many(t) { 25 }; m.many(t) { 26 }; m.many(t) { 27 }; m.many(t) { 28 }; m.many(t) { 29 }; m.many(t) { 30 };
m.many(t) { 31 }; m.many(t) { 32 }; m.many(t) { 33 }; m.many(t) { 34 }; m.many(t) { 35 }; m.many(t) { 36 }; m.many(t) { 37 }; m.many(t) { 38 }; m.many(t) { 39 }; m.many(t) { 40 };
m.many(t) { 41 }; m.many(t) { 42 }; m.many(t) { 43 }; m.many(t) { 44 }; m.many(t) { 45 }; m.many(t) { 46 }; m.many(t) { 47 }; m.many(t) { 48 }; m.many(t) { 49 }; m.many(t) { 50 };
m.many(t) { 51 }; m.many(t) { 52 }; m.many(t) { 53 }; m.many(t) { 54 }; m.many(t) { 55 }; m.many(t) { 56 }; m.many(t) { 57 }; m.many(t) { 58 }; m.many(t) { 59 }; m.many(t) { 60 };
m.many(t) { 61 }; m.many(t) { 62 }; m.many(t) { 63 }; m.many(t) { 64 }; m.many(t) { 65 }; m.many(t) { 66 }; m.many(t) { 67 }; m.many(t) { 68 }; m.many(t) { 69 }; m.many(t) { 70 };
p m.many(t) { @flag = nil }

# a String splice nested in one whose slot holds an Integer's nil
class U
  def initialize = (@s = "a")
end
uu = U.new
t.flag = 4
p(t.instance_exec do
  x = uu.instance_exec { next nil if @s == "a"; "s" }
  p x
  next nil if @flag == 4
  1
end)
