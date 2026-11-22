# Signatures with a Proc type (`^(T) -> U`) in a parameter or return slot.
# The Proc slot is not seeded -- it stays inferred, as --rbs leaves it --
# but the other parameter and return types still apply, and the program
# compiled with `--no-inline-rbs --rbs sig/proc_types` produces the same C
# byte for byte. Each annotation pins a slot to `untyped`, which inference
# would not choose on its own, so the C shows whether it applied.
class Bus
  #: () -> void
  def initialize
    @handlers = []
  end

  #: (untyped, ^(Integer) -> void) -> untyped
  def on(name, handler)
    @handlers << handler
    name
  end

  #: (untyped) -> untyped
  def fire(x)
    @handlers.each { |h| h.call(x) }
    @handlers.length
  end

  #: (untyped, (^(String) -> String)?) -> untyped
  def shout(s, fmt)
    fmt ? fmt.call(s) : s
  end

  #: (untyped) -> ^(Integer) -> Integer
  def adder(n) = ->(x) { x + n }

  # @rbs tag: untyped
  # @rbs cb: ^(String) -> void
  # @rbs return: untyped
  def each_tag(tag, cb)
    cb.call(tag)
    tag
  end
end

b = Bus.new
p b.on("a", ->(x) { print x, " " })
p b.on("b", ->(x) { print x * 2, " " })
p b.fire(3)
p b.shout("hi", ->(s) { s.upcase })
p b.shout("hi", nil)
p b.adder(10).call(5)
p b.each_tag("t", ->(s) { print s, " " })
