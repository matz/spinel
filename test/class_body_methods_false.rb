# A class's own method lists asked of self in its body, and of the class by
# name: `methods(false)` and `singleton_methods(false)` are its public and
# protected class methods, `private_methods(false)` its private ones beside
# Class's own (inherited, initialize), as CRuby lists them. activesupport's
# CurrentAttributes computes a constant of every name at the end of its body.
class Current
  def self.a; end
  class << self
    def b; end
    private def c; end
    attr_reader :d
    protected def g; end
  end
  def e; end
  private def f; end
  p methods(false).sort
  p private_methods(false).sort
  p instance_methods(false).sort
  p private_instance_methods(false).sort
  p singleton_methods(false).sort
  INVALID_ATTRIBUTE_NAMES = (methods(false) + private_methods(false) + instance_methods(false) + private_instance_methods(false)).uniq.sort.freeze
end
p Current::INVALID_ATTRIBUTE_NAMES
p Current.methods(false).sort
p Current.private_methods(false).sort
p Current::INVALID_ATTRIBUTE_NAMES.include?(:a), Current::INVALID_ATTRIBUTE_NAMES.include?(:zz)

class Plain
  def self.inherited(sub); super; end
  p methods(false), private_methods(false).sort
end
