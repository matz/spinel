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
