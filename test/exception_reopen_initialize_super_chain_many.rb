# Nine reopenings that define initialize come before NameError's: the lookup of the
# initialize a reopened NoMethodError's super reaches must not stop at a fixed number
# of candidates (it did at eight, and skipped the reopened NameError#initialize).
class ArgumentError
  def initialize(m = nil) = super("A:" + m.to_s)
end
class TypeError
  def initialize(m = nil) = super("T:" + m.to_s)
end
class IOError
  def initialize(m = nil) = super("I:" + m.to_s)
end
class IndexError
  def initialize(m = nil) = super("I:" + m.to_s)
end
class RangeError
  def initialize(m = nil) = super("R:" + m.to_s)
end
class ZeroDivisionError
  def initialize(m = nil) = super("Z:" + m.to_s)
end
class EOFError
  def initialize(m = nil) = super("E:" + m.to_s)
end
class StopIteration
  def initialize(m = nil) = super("S:" + m.to_s)
end
class RuntimeError
  def initialize(m = nil) = super("R:" + m.to_s)
end
class NameError
  def initialize(msg = nil, *rest, **kw) = super("N: #{msg}", *rest, **kw)
end
class NoMethodError
  def initialize(m = nil) = super("M: " + m.to_s)
end
p NoMethodError.new("x").message
