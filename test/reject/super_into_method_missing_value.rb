# A super into a method whose own super reaches no method: CRuby calls the
# receiver's method_missing there, here one a subclass defines, and its
# value is the String. Spinel does not dispatch method_missing, so the
# super is refused rather than read as nil after a NoMethodError.
# spinel: reject-unsupported: own super reaches method_missing is not supported
class P
  def g = super
end
class C < P
  def g = ["c", super]
end
class D < C
  def method_missing(name, *args) = "mm:#{name}"
  def respond_to_missing?(name, priv = false) = true
end
p D.new.g
