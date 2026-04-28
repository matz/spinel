# `def x=(v); @x = v * 2; end` looks like an attr_writer at first glance
# (single InstanceVariableWriteNode body) but the assignment value is a
# computation, not a bare param reference — `is_simple_writer_method`
# must not classify it as auto-attr_writer.
#
# Without that fix, the method gets auto-registered in @cls_attr_writers,
# `cls_has_attr_writer(C, "doubled")` returns true, and the call site
# short-circuits `c.doubled = 5` to `c->iv_doubled = 5` — bypassing the
# `* 2` entirely. Ruby would print 10; pre-fix Spinel printed 5.

class C
  def initialize
    @doubled = 0
  end

  def doubled=(v)
    @doubled = v * 2
  end

  def get_doubled
    @doubled
  end
end

c = C.new

c.doubled = 5
puts c.get_doubled    # 10  (5 * 2)

c.doubled = -7
puts c.get_doubled    # -14

puts "done"
