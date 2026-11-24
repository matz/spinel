# A respond_to_missing? defined at the top level is Object's private method,
# which the splat's conversion asks (directly, as CRuby does): it says no, so
# the object spreads itself and method_missing's to_a is not called.
def respond_to_missing?(n, priv = false) = false

class C
  def method_missing(n, *a) = n == :to_a ? [:conv] : super
end

p [*C.new].map(&:class)
