# The copy of a define_method original an alias makes keeps the original's
# names: __method__ and the owner in a rendering; __callee__ is the
# alias's. A class-side copy of a replaced body renders the owner after the
# receiver's class.
class A
  define_method(:d) { [__method__, __callee__] }
  def self.s = [__method__, __callee__]
end
class B < A
  alias_method :e, :d
  class << self
    alias t s
  end
end
class A
  def d(x = 0) = :new
  def self.s = :new
end
p B.new.e
puts B.instance_method(:e).inspect.sub(/ \S+:\d+>\z/, ">")
puts B.new.method(:e).inspect.sub(/ \S+:\d+>\z/, ">")
p B.t
puts B.method(:t).inspect.sub(/ \S+:\d+>\z/, ">")
