# A bare super from a `(...)` method into a builtin's method passes the
# keywords its calls give as one positional Hash, which the builtin counts:
# a count it does not take raises CRuby's ArgumentError.

def run
  yield
rescue ArgumentError, TypeError => e
  puts "#{e.class} #{e.message}"
end

class K
  def is_a?(...) = super
  def instance_of?(...) = super
  def respond_to?(...) = super
  def foo; end
  def self.superclass(...) = super
  def self.subclasses(...) = super
  def self.ancestors(...) = super
end
class L < K; end
k = K.new
run { p k.is_a?(K, k: 1) }
run { p [k.instance_of?(K, k: 1), :after] }
run { p k.respond_to?(:foo, k: 1) }
run { p K.superclass(k: 1) }
run { p L.subclasses(a: 1) }
run { p(K.ancestors(a: 1) { 2 }) }
x = begin; L.superclass(k: 1); rescue ArgumentError; :raised; end
p x
