# instance_exec, instance_eval and then answer their block's value. Where
# that is a nil-able Integer slot of the receiver -- `@flag = nil` written
# in the block, or `@flag` read while it is nil -- the method answering
# the call was given a plain Integer return, and the nil raised
# "no implicit conversion from nil to integer" on the way out.
class T
  attr_accessor :flag
  def initialize = (@flag = 3)
end

class U
  def initialize = (@n = nil)
  def set = (@n = 4)
end

class M
  def mark(t) = t.instance_exec { @flag = nil }
  def mark_eval(t) = t.instance_eval { @flag = nil }
  def pick(t, v) = t.instance_exec { @flag = v ? 5 : nil }
  def look(u) = u.instance_exec { @n }
  def via_then(t) = t.then { |x| x.flag }
end

t = T.new
p M.new.mark(t), t.flag
t.flag = 3
p M.new.mark_eval(t), t.flag
p M.new.pick(t, false), M.new.pick(t, true), t.flag
u = U.new
p M.new.look(u)
u.set
p M.new.look(u)
t.flag = nil
p M.new.via_then(t)
t.flag = 8
p M.new.via_then(t) + 1
