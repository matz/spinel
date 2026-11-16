# --emit-types says whether a value whose kind has a nil form of its own can
# be nil: the nullable marking for an Integer or Float (#3505), the nil fact
# for an object or a pointer-held builtin (#7444). A consumer that unboxes on
# the strength of the type needs this beside the type, since `Integer` and
# `Integer?` are one TY_INT. The emit-types leg of infer-test reads this.
def find(a, k)
  i = a.index(k) || :none
  i
end
def total(a)
  s = 0
  a.each { |x| s += x }
  s
end
def first_name(names)
  names.first
end
def label(n)
  "item #{n}"
end
xs = [1, 2, 3]
p find(xs, 2)
p total(xs)
p first_name(["a", "b"])
p label(1)
