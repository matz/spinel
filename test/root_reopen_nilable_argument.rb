# An argument that can be nil marks a program's Object method's parameter
# nullable, so the method answers an Integer that can be nil: a call on nil,
# on an Integer or on a String reads that answer in its own form.
class Object; def foo(x) = x; end
p(nil.foo(nil))
p(3.foo(1))
p(3.foo(nil))
p("s".foo(nil))
p(nil&.foo(nil))
w = nil
p(w&.foo(1))
r = 4.foo(nil)
p r.nil?
