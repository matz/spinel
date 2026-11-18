# A Method local that a write names one method for, and that a multiple
# assignment or a parameter's argument gives another: each use of it
# answers for the Method it holds (method_capture_boxed_arg_hidden_write.rb
# is its call with a boxed argument).
class Parent
  def f(x) = [:parent_f, x]
end
class Child < Parent
  def f(x) = [:child_f, x]
end
class OtherParent
  def g(x) = [:other_parent_g, x]
end
class Other < OtherParent
  def g(x) = [:other_g, x]
end
CHILD = Child.new
OTHER = Other.new
def other = OTHER.method(:g)
def held0
  m = CHILD.method(:f)
  spare, m = 0, other
  m.call(1)
end
def passed0(m, flag)
  m = CHILD.method(:f) if flag
  m.call(1)
end
def held1
  m = CHILD.method(:f)
  spare, m = 0, other
  m[2]
end
def passed1(m, flag)
  m = CHILD.method(:f) if flag
  m[2]
end
def held2
  m = CHILD.method(:f)
  spare, m = 0, other
  m === 3
end
def passed2(m, flag)
  m = CHILD.method(:f) if flag
  m === 3
end
def held3
  m = CHILD.method(:f)
  spare, m = 0, other
  m.owner
end
def passed3(m, flag)
  m = CHILD.method(:f) if flag
  m.owner
end
def held4
  m = CHILD.method(:f)
  spare, m = 0, other
  m.name
end
def passed4(m, flag)
  m = CHILD.method(:f) if flag
  m.name
end
def held5
  m = CHILD.method(:f)
  spare, m = 0, other
  m.original_name
end
def passed5(m, flag)
  m = CHILD.method(:f) if flag
  m.original_name
end
def held6
  m = CHILD.method(:f)
  spare, m = 0, other
  m.arity
end
def passed6(m, flag)
  m = CHILD.method(:f) if flag
  m.arity
end
def held7
  m = CHILD.method(:f)
  spare, m = 0, other
  m.parameters
end
def passed7(m, flag)
  m = CHILD.method(:f) if flag
  m.parameters
end
def held8
  m = CHILD.method(:f)
  spare, m = 0, other
  m.to_proc.call(4)
end
def passed8(m, flag)
  m = CHILD.method(:f) if flag
  m.to_proc.call(4)
end
def held9
  m = CHILD.method(:f)
  spare, m = 0, other
  m.to_proc.arity
end
def passed9(m, flag)
  m = CHILD.method(:f) if flag
  m.to_proc.arity
end
def held10
  m = CHILD.method(:f)
  spare, m = 0, other
  m.curry[5]
end
def passed10(m, flag)
  m = CHILD.method(:f) if flag
  m.curry[5]
end
def held11
  m = CHILD.method(:f)
  spare, m = 0, other
  [m == CHILD.method(:f), m == OTHER.method(:g)]
end
def passed11(m, flag)
  m = CHILD.method(:f) if flag
  [m == CHILD.method(:f), m == OTHER.method(:g)]
end
def held12
  m = CHILD.method(:f)
  spare, m = 0, other
  m.source_location[1]
end
def passed12(m, flag)
  m = CHILD.method(:f) if flag
  m.source_location[1]
end
def held13
  m = CHILD.method(:f)
  spare, m = 0, other
  [6, 7].map(&m)
end
def passed13(m, flag)
  m = CHILD.method(:f) if flag
  [6, 7].map(&m)
end
def held14
  m = CHILD.method(:f)
  spare, m = 0, other
  m.dup.call(8)
end
def passed14(m, flag)
  m = CHILD.method(:f) if flag
  m.dup.call(8)
end
def held15
  m = CHILD.method(:f)
  spare, m = 0, other
  m.clone.call(9)
end
def passed15(m, flag)
  m = CHILD.method(:f) if flag
  m.clone.call(9)
end
def held16
  m = CHILD.method(:f)
  spare, m = 0, other
  m.unbind.name
end
def passed16(m, flag)
  m = CHILD.method(:f) if flag
  m.unbind.name
end
p held0, passed0(other, false), passed0(other, true)
p held1, passed1(other, false), passed1(other, true)
p held2, passed2(other, false), passed2(other, true)
p held3, passed3(other, false), passed3(other, true)
p held4, passed4(other, false), passed4(other, true)
p held5, passed5(other, false), passed5(other, true)
p held6, passed6(other, false), passed6(other, true)
p held7, passed7(other, false), passed7(other, true)
p held8, passed8(other, false), passed8(other, true)
p held9, passed9(other, false), passed9(other, true)
p held10, passed10(other, false), passed10(other, true)
p held11, passed11(other, false), passed11(other, true)
p held12, passed12(other, false), passed12(other, true)
p held13, passed13(other, false), passed13(other, true)
p held14, passed14(other, false), passed14(other, true)
p held15, passed15(other, false), passed15(other, true)
p held16, passed16(other, false), passed16(other, true)
