# The owner of a Method of a `true` or `false` literal is the module
# prepended to its class that defines the method; an included module's
# method stands behind the class's own.
module Amp
  def &(o) = :amp
end
module Insp
  def inspect = "I"
end
module Str
  def to_s = "S"
end
class TrueClass
  prepend Amp
  include Insp
end
class FalseClass
  prepend Amp
  prepend Str
end
p true.method(:&).owner
p true.method(:&).call(1)
p [false.method(:&), 1][0].owner
p true.method(:|).owner
p true.method(:inspect).owner
p [true.method(:inspect), 1][0].owner
p false.method(:to_s).owner
p false.method(:to_s).call
p true.method(:to_s).owner
p true.method(:to_s).call
