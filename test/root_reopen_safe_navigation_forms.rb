# `&.` on a receiver that is not known to be nil: the program's Object method
# runs on a value, the call answers nil on a nil, and the arguments of a
# call on nil are not evaluated.
class Object; def foo = 1; def bar(x) = x; end
class NilClass; def baz(x) = x; end
p(5&.foo)
p("s"&.foo)
v = [nil, 2].last
p(v&.foo)
w = [2, nil].last
p(w&.foo)
def m(x) = x&.foo
p m(nil)
p m(3)
p(nil&.bar(puts("evaluated")))
p(nil&.baz(puts("evaluated2")))
u = nil
p(u&.bar(1))
