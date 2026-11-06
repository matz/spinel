# A `next` or `break` in an instance_exec / instance_eval block over an
# object answers the splice with its value, nil included: a `next` that is
# the block's only statement, `break nil`, a bare `break`.
class T
  attr_accessor :flag, :f
  def initialize = (@flag = 4; @f = 1.5)
  def fire(&b) = instance_exec(&b)
end

class M
  def look(t, &b) = t.instance_exec(&b)
end

t = T.new

# a `next` or `break` as the block's only statement
p t.instance_eval { next @flag > 3 ? nil : 2 }
p t.instance_eval { next @flag > 5 ? nil : 2 }
p t.instance_eval { next }
p t.instance_eval { next 3 }
p t.instance_eval { break }
p t.instance_eval { break 3 }
p t.instance_exec { break @flag > 3 ? nil : 2 }

# `break nil`, a bare `break`, and a `break` that does not run
p t.instance_exec { break nil if @flag == 4; 1 }
p t.instance_exec { break if @flag == 4; 1 }
p t.instance_exec { break nil if @flag == 5; 1 }
p t.instance_exec { break 7 if @flag == 4; 1 }
p t.instance_exec { next if @flag == 4; 1 }
p t.instance_exec(3) { |a| break nil if a == 3; a + 1 }
p t.instance_exec(2) { |a| break nil if a == 3; a + 1 }

# a Float slot
p t.instance_eval { break @f > 1 ? nil : 2.5 if @flag == 4; 1.0 }
p t.instance_eval { break if @flag == 4; @f }
p t.instance_eval { break if @flag == 5; @f }

# the value held, returned by a method, used in arithmetic
x = t.instance_exec { break nil if @flag == 4; 1 }
p x
y = t.instance_exec { break if @flag == 5; 1 }
p y + 1
def m_next(t) = t.instance_eval { next @flag > 3 ? nil : 2 }
def m_break(t) = t.instance_exec { break nil if @flag == 4; 1 }
def m_bare(t) = t.instance_exec { break if @flag == 4; 1 }
def m_float(t) = t.instance_exec { break if @flag == 4; @f }
p m_next(t), m_break(t), m_bare(t), m_float(t)
p((t.instance_exec { break nil if @flag == 4; 1 } || 0) + 1)
p((t.instance_exec { break 3 if @flag == 4; 1 }) + 1)

# a break of a loop or block inside the block leaves that one
p t.instance_exec { i = 0; while i < 3; i += 1; break if i == 2; end; i }
p t.instance_exec { [1, 2].each { |v| break nil if v == 1 }; 5 }
p t.instance_exec { [1, 2].map { |v| next nil if v == 1; v }.size }
p t.instance_exec { loop { break nil }; 3 }

# a splice in a splice
p t.instance_exec { t.instance_exec { break nil if @flag == 4; 1 }; 2 }
p t.instance_exec { t.instance_exec { break nil if @flag == 4; 1 } }
p t.instance_exec { t.instance_exec { break "x" if @flag == 4; "y" } }

# an ensure the break leaves runs
p t.instance_exec { begin; break nil if @flag == 4; 1; ensure; puts "ensure"; end }

# a trampoline's block, and a block passed on to instance_exec
p t.fire { break nil if @flag == 4; 1 }
p t.fire { break }
p t.fire { next @flag > 3 ? nil : 2 }
p M.new.look(t) { break nil if @flag == 4; 1 }
p M.new.look(t) { next nil if @flag == 4; 1 }
p M.new.look(t) { next nil }
p M.new.look(t) { next 3 }

# other kinds of value
p t.instance_exec { break "s" if @flag == 4; "t" }
p t.instance_exec { break nil if @flag == 4; "t" }
p t.instance_exec { break [1] if @flag == 4; [2] }
p t.instance_exec { break nil if @flag == 4; [2] }
p t.instance_exec { break nil if @flag == 4; :s }
