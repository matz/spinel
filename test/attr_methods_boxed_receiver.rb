# A boxed receiver's `methods` or `public_methods` calls the method or reader
# of the name its class has, and lists the methods of other values (boxcar's
# Router::Route had `attr_reader :methods`)
class Route
  attr_reader :methods

  def initialize(methods)
    @methods = methods
  end
end

class Gate
  attr_accessor :public_methods
end

class Verb
  def methods
    "verb"
  end
end

class Plain; end

[Route.new(["GET"]), Route.new(["POST", "PUT"])].each do |route|
  p route.methods
end

g = Gate.new
g.public_methods = [:open]
[g].each { |x| p x.public_methods }

r = Route.new(["HEAD"])
p r.methods

[Route.new(["GET"]), Verb.new, Plain.new, 1, "s", nil].each do |o|
  m = o.methods
  p(m.is_a?(Array) && m.include?(:to_s) ? :list : m)
end
