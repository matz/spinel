# A builtin exception with no reopening of its own, built under a reopened
# builtin that defines initialize (StandardError#initialize below), runs that
# initialize: `.new`, `raise Cls`, `raise Cls, msg` and `raise "msg"` all
# construct it as CRuby does. They built the runtime's exception and skipped
# the reopening unless the class itself had been reopened.
class StandardError
  def initialize(msg = nil) = super("S[#{msg}]")
end
p ArgumentError.new("a").message, TypeError.new("t").message, IOError.new("i").message
p IndexError.new("i").message, StopIteration.new("s").message, ZeroDivisionError.new("z").message
p FrozenError.new("f").message, KeyError.new("k").message, NameError.new("n").message, NoMethodError.new("m").message
p ArgumentError.new.message, RuntimeError.new("r").message
# not a StandardError: the reopening is not above them
p NotImplementedError.new("n").message, SystemExit.new(1, "q").message, Interrupt.new("u").message
begin; raise ArgumentError, "a"; rescue => e; p e.message, e.class; end
begin; raise ArgumentError; rescue => e; p e.message; end
begin; raise TypeError.new("b"); rescue => e; p e.message; end
begin; raise "plain"; rescue => e; p e.message, e.class; end
begin; raise KeyError, "kk"; rescue => e; p e.message, e.class; end
x = ArgumentError.new("v")
p x.message, x.class, x.is_a?(StandardError)

# a reopening of the class itself comes first, and its super reaches the one above
class TypeError
  def initialize(msg = nil) = super("T:" + msg.to_s)
end
p TypeError.new("t").message
begin; raise TypeError, "tt"; rescue => e; p e.message; end
