# respond_to? answers for a method added by a reopened Object, Kernel or
# BasicObject: a private or protected one is found only with the second
# argument true; a class that undefines the name or defines its own keeps
# its own answer. The name can be known at compile time or at run time.
class Object
  private def top = "t"
  protected def pro = 1
  def pubm = 2
end
module Kernel; private def kk = 1; def kp = 2; end
class BasicObject; def bb = 3; end
class A; end
class B; def top = "b"; end
class C < A; undef_method :pubm; end
S = Struct.new(:x)
vals = [A.new, B.new, C.new, 3, "s", nil, :y, [1], {a: 1}, 1.5, S.new(1), A, Object.new, (1..2)]
names = [:top, :pro, :pubm, :kk, :kp, :bb]
vals.each do |v|
  p names.map { |n| [v.respond_to?(n), v.respond_to?(n, true)] }
end
names.each do |n|
  m = [n].first
  p vals.map { |v| [v.respond_to?(m), v.respond_to?(m, true)] }
end
p A.new.respond_to?(:top), A.new.respond_to?(:top, true)
p C.new.respond_to?(:pubm), B.new.respond_to?(:top)
p 3.respond_to?(:top, true), "s".respond_to?(:pro, true), 3.respond_to?(:pro)
