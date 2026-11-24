# A respond_to_missing? or respond_to? the program defines after a splat has
# run is not asked there: the instance answers as if it had none.
#
# Inside a method that runs once before the defs (A, B), the plan cannot tell
# whether the hook exists at every run, so the splat stays as it was: the
# object spreads itself, which is right with a method_missing that raises
# NoMethodError for to_a.
#
# At the top level (D) the hook is absent over the whole window, so the
# conversion runs as for a class with no hook: method_missing(:to_a) answers.
class A
  def method_missing(n, *a) = n == :to_a ? raise(NoMethodError, "inner") : super
end

class B
  def method_missing(n, *a) = n == :to_a ? raise(NoMethodError, "inner") : super
end

class D
  def method_missing(n, *a) = n == :to_a ? [:conv] : super
end

def sp(x) = [*x]

p sp(A.new).map(&:class)
p sp(B.new).map(&:class)
p [*D.new]

class A
  def respond_to_missing?(n, priv = false) = n == :to_a
end

class B
  def respond_to?(n, priv = false) = n == :to_a
end

class D
  def respond_to_missing?(n, priv = false) = false
end
