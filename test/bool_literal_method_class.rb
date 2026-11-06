# A Method of a `true` or `false` literal is of that value's class: a
# reopen of TrueClass gives true's Method its def (parameters, arity), and
# the value's own methods name TrueClass or FalseClass as their owner.
class TrueClass
  def to_s(custom = nil) = "T#{custom}"
end
class FalseClass
  def inspect = "F"
end

m = true.method(:to_s)
p m.parameters
p m.arity
p m.call
p m.call(1)
p m.owner

p false.method(:to_s).owner
p false.method(:to_s).call
p false.method(:inspect).call
p false.method(:inspect).owner
p true.method(:inspect).call

# through a container, a conditional, an `||` and a write
p [true.method(:to_s), 1][0].owner
p [false.method(:&), 1][0].owner
p (1 > 0 ? true : true).method(:to_s).parameters
p (false || true).method(:to_s).owner
p (y = false).method(:to_s).owner
p begin; true; end.method(:to_s).owner

# the methods of every object keep their owner
p true.method(:frozen?).owner
p false.method(:==).owner
p true.method(:^).owner
p false.method(:|).call(true)
