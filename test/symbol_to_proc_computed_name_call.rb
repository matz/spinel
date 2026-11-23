# A Symbol made from a String at run time, turned into a proc and called.
# The proc's dispatch has an arm for every method the name may be, one of
# them Integer#between?, whose bounds come boxed out of the proc's
# arguments: the plain compare was written against the box, and the
# generated C did not compile.
def via(s, *args) = (begin; s.to_sym.to_proc.call(*args); rescue NoMethodError; :nm; rescue ArgumentError; :arg; end)
def one(s) = (begin; s.to_sym.to_proc.call(1); rescue NoMethodError; :nm; end)
p one("nope"), one("succ"), one("to_s")
p via("between?", 5, 1, 9), via("between?", 5, 6, 9), via("between?", 2.5, 1, 3)
p via("clamp", 12, 1, 9), via("abs", -4), via("nope", 1)
