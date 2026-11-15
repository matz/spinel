# A bare super from an exception's initialize that takes a rest parameter
# hands on the rest's elements, as `super(*rest)` does: the first is the
# message, none leaves the class name, two raise ArgumentError, and the
# keywords of a **rest reach NoMethodError's receiver: and KeyError's key:.
# The rest Array itself was stored as the message, and the C did not
# compile (#11889).
class AppError < StandardError
  def initialize(*args)
    super
  end
end
p AppError.new.message
p AppError.new(nil).message
p AppError.new(:sym).message
begin; AppError.new("a", "b"); rescue ArgumentError => e; p [e.class, e.message]; end
class TwoError < StandardError
  def initialize(m, *rest)
    super
  end
end
p TwoError.new("m").message
begin; TwoError.new("m", 1); rescue ArgumentError => e; p [e.class, e.message]; end
class KwError < StandardError
  def initialize(*a, **k)
    super
  end
end
p KwError.new("k").message
class NmError < NoMethodError
  def initialize(*a, **k)
    super
  end
end
e1 = NmError.new("nm", :foo, receiver: 5)
p [e1.message, e1.name, e1.receiver]
class KyError < KeyError
  def initialize(*a, **k)
    super
  end
end
e2 = KyError.new("ky", key: :z, receiver: {})
p [e2.message, e2.key]
class RuntimeError
  def initialize(*args)
    super
  end
end
begin; raise "boom"; rescue => e3; p e3.message; end
p RuntimeError.new.message
