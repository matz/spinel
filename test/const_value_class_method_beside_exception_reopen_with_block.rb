# The same with a reopened StandardError: a class no switch arm names (a builtin exception) is asked about the call's lowering, and the arms the emitter may still write keep their versions.
class StandardError
  def self.hi(&b) = b ? b.call(9) : 0
end
class A; def self.hi = 1; end
class B; end
class Other; def self.hi = 5; end
S = [1, 2].sum == 3 ? A : B
p S.hi
class Other; def self.hi = 6; end
p Other.hi
