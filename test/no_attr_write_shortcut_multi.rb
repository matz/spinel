# `obj.x = v` must dispatch to `def x=(v)` when x= is not a registered
# attr_writer. A multi-statement body never matched the auto-attr_writer
# pattern (which requires a single `InstanceVariableWriteNode` body), so
# the fix at the call site is sufficient on its own to make this case
# work — no auto-classification change required.

class C
  attr_accessor :real

  def initialize
    @real = 0
    @logged = ""
  end

  # Multi-statement writer. Side-effects on a *different* ivar so we can
  # tell whether the def actually ran.
  def logged=(v)
    @logged = "set:" + v
    @real = v.length
  end

  def get_logged
    @logged
  end
end

c = C.new

# attr_accessor path: still short-circuits to field write.
c.real = 7
puts c.real           # 7

# Multi-statement def x=: must dispatch (not bypass).
c.logged = "hello"
puts c.get_logged     # set:hello
puts c.real           # 5  (overwritten by the side effect)

puts "done"
