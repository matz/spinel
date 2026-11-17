# An alias of a builtin's method is a method of the class: respond_to? answers
# true for it on a typed receiver, on a boxed one and on the members of an
# array of mixed values, as it does for an alias of the program's own method.
class A < Array; alias_method :sz, :size; alias cnt count; end
class H < Hash; alias_method :ks, :keys; end
class E < StandardError; alias_method :mmm, :message; alias bt backtrace; end
class P; alias_method :ins, :inspect; alias fr frozen?; def mine = 1; alias_method :mine2, :mine; end

a = A.new([1])
h = H.new
e = E.new("x")
q = P.new
p a.respond_to?(:sz), a.respond_to?(:cnt), a.respond_to?(:zz)
p h.respond_to?(:ks), e.respond_to?(:mmm), e.respond_to?(:bt)
p q.respond_to?(:ins), q.respond_to?(:fr), q.respond_to?(:mine2), q.respond_to?(:nope)

mixed = [a, h, e, q, 3, "s", nil]
%i[sz cnt ks mmm bt ins fr mine2 nope].each do |n|
  p [n, mixed.map { |o| o.respond_to?(n) ? 1 : 0 }.join]
end
p mixed.map { |o| o.respond_to?(:fr) }
def ask(o) = o.respond_to?(:fr)
p ask(q), ask(mixed[0]), ask(3)
