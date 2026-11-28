# A method a subclass overrides, called on an instance of the parent class,
# where the subclass itself is never made: the plan binds the call to the
# parent's method alone (no instance of the subclass can be the receiver), so
# the call's type is that method's answer, not the join with the override's
# (which read the parent's object as a boxed value and did not compile).
class A
  def m
    A.new
  end
end
class B < A
  def m
    B.new
  end
end
def f(x) = x.m
x = A.new
p x.m.class
p f(x).class
p [x.m, A.new].first.class
