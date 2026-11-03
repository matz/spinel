# `Kernel.sleep(x)` in a module's own `def self.sleep` calls Kernel's sleep,
# which answers an Integer. With the receiver dropped, the call was typed by
# the module's method of the same name (an untyped return), while the
# emitter called Kernel's: the method returned the Integer unboxed where it
# answers a box, and the C did not build. format, rand and Integer are the
# same.
module Clock
  def self.sleeper=(callable)
    @sleeper = callable
  end

  def self.sleeper = @sleeper

  def self.sleep(seconds = 0)
    s = sleeper
    return s.call(seconds) if s

    Kernel.sleep(seconds.to_f)
  end
end

module Text
  def self.format(fmt, v) = v ? Kernel.format(fmt, v) : :none
  def self.rand(n) = n ? Kernel.rand(n) : :none
  def self.Integer(s) = s ? Kernel.Integer(s) : :none
end

p Clock.sleep(0)
Clock.sleeper = ->(x) { "slept #{x}" }
p Clock.sleep(1)
p Clock.sleep(0.5)
p Text.format("%05.1f", 2.5)
p Text.format("%d", nil)
p Text.rand(1)
p Text.rand(nil)
p Text.Integer("42")
p Text.Integer(nil)
