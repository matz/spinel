# A class that overrides respond_to? is asked it first, with the one argument
# a one-parameter def takes; with respond_to_missing? overridden too, both must
# say yes before method_missing(:to_a) runs. (Kept apart from the other splat
# test: a respond_to? defined anywhere makes the builtin one skip a class's
# own respond_to_missing?.)
class BothRT
  def respond_to?(n, priv = false) = n == :to_a ? true : super
  def respond_to_missing?(n, priv = false) = false
  def method_missing(n, *a) = n == :to_a ? [:a, :b] : super
end

class OneArgRT
  def respond_to?(n) = n == :to_a
  def method_missing(n, *a) = n == :to_a ? [:one, :arg] : super
end

class NoRT
  def respond_to?(n, priv = false) = false
  def method_missing(n, *a) = n == :to_a ? [:never] : super
end

p [*BothRT.new].map(&:class)
p [*OneArgRT.new]
p [*NoRT.new].map(&:class)

# The include-private flag goes to every respond_to? whose arity is not exactly 1.
class KwRT
  def respond_to?(n, **kw) = true
  def method_missing(n, *a) = n == :to_a ? [:conv] : super
end
begin
  p [*KwRT.new]
rescue ArgumentError => e
  p e.class
end
