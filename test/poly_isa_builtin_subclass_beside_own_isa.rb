# is_a? on a boxed instance of a Hash or Array subclass, in a program where
# another class defines its own is_a? (so the call dispatches on the
# receiver's class): the instance answers its own class, its superclasses,
# the builtin and the modules it includes, as it does without that class.

class Headers
  def initialize(h) = @h = h
  def is_a?(k) = k == Hash || super
end

module Tagged; end

class Params < Hash
  include Tagged
end
class DeepParams < Params; end
class Row < Array; end

def check(o, k) = o.is_a?(k)

vals = [Params.new, DeepParams.new, Row.new, Headers.new({}), {}, [], "s", nil]
vals.each do |v|
  p [v.class, v.is_a?(Params), v.is_a?(DeepParams), v.is_a?(Hash), v.is_a?(Tagged),
     v.is_a?(Row), v.is_a?(Array), v.is_a?(Enumerable), v.is_a?(Object), v.instance_of?(Params)]
  p [check(v, Params), check(v, Hash), check(v, Row), check(v, Array), check(v, Tagged)]
end

# the shape of a merge whose argument is the receiver's own class
class Params
  def absorb(other) = other.is_a?(Params) ? :params : other.is_a?(Hash) ? :hash : :other
end
p Params.new.absorb(vals[0])
p Params.new.absorb(vals[1])
p Params.new.absorb({})
p Params.new.absorb(vals[3])
p Params.new.absorb(vals[2])
