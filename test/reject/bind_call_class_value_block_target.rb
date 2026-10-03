# A bind_call on a Class value known only at run time switches on its class,
# one arm per class that defines the method. A target that takes a block has
# no arm (the arms pass none), so the call is refused. The call plan decides
# it from the class table (cplan_bind_call_gap).
class A
  def hi; "A"; end
end
class B < A
  def hi(&b); "B"; end
end
k = [A, B].sample
p k.instance_method(:hi).bind_call(B.new)
