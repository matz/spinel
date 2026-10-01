# A Method called with more than 8 arguments: the synthesized wrapper that
# carries a method(:sym) value held at most 8 forwarded parameters, and past
# that it took none, so the arguments never reached the method.

class A; def w(*r) = r.size; end
class B; def w(*r) = 0; end
class C; def w(a, b, c, d, e, f, g, h, i) = [a, i]; end

# a boxed receiver: the method sees all nine
i = [A.new, B.new][ARGV.size]
p i.method(:w).call(1, 2, 3, 4, 5, 6, 7, 8, 9)
p i.method(:w).(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12)

# nine required parameters, all bound
j = [C.new, A.new][ARGV.size]
p j.method(:w).call(1, 2, 3, 4, 5, 6, 7, 8, 9)

# exactly 8 kept its fixed wrapper
p i.method(:w).call(1, 2, 3, 4, 5, 6, 7, 8)

# a Kernel function printed only its first argument
pm = method(:puts)
pm.call(1, 2, 3, 4, 5, 6, 7, 8, 9)

# a builtin on a boxed receiver
x = [[0], "s"][ARGV.size]
p x.method(:push).call(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
p [1, 2].method(:values_at).call(0, 1, 0, 1, 0, 1, 0, 1, 0, 1)
