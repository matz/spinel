# A `super` from the initialize of a reopened builtin exception reaches the
# initialize a reopening of the builtin above it defines (NoMethodError's, the
# NameError's), and that one's own super reaches the next, as CRuby's lookup
# does. The builtin's was reached directly, and the middle one never ran.
class NameError
  def initialize(msg = nil, *rest, **kw) = super("N: #{msg}", *rest, **kw)
end
class NoMethodError
  def initialize(m = nil) = super("M: " + m.to_s)
end
p NoMethodError.new("x").message
p NameError.new("y").message

# a bare super hands on its own parameters
class IndexError
  def initialize(msg = nil) = super("I:#{msg}")
end
class KeyError
  def initialize(msg = nil)
    super
  end
end
p KeyError.new("k").message
p IndexError.new("i").message

# a class of the program below a reopened builtin: its super goes through
# the reopening chain
class MyNM < NoMethodError
  def initialize(m = nil) = super("my:" + m.to_s)
end
p MyNM.new("z").message

begin
  raise NoMethodError, "boom"
rescue NoMethodError => e
  p e.message
end
