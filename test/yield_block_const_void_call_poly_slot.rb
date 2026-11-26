# A rescuing method given, as blocks, a call of a class method that only
# raises (`C.unknown`) and the call of a lambda that ends in it, where a
# lambda returning an object is stored later: the yield's slot is boxed,
# and the void call's block takes the boxed nil, not the String's.
class Rel
  def initialize(t); @t = t; end
end
module C
  def self.unknown(name)
    raise "no #{name}"
  end
  def self.no_builder = ->(name) { C.unknown(name) }
  @b = C.no_builder
  def self.b=(v)
    @b = v
  end
end
def rel_builder = ->(name) { Rel.new(name) }
C.b = rel_builder
def g
  yield
rescue StandardError => e
  e.message
end
out = Array.new(0) { "" }
out << g { C.unknown("a") }
out << g { C.no_builder.call("b") }
p out
