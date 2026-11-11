# A prepended module brings the modules it includes (and prepends) into
# the class's ancestors: ancestors, is_a?, ===, when, include?, <, rescue.
module A; def a_m = :a; end
module B; include A; end
module C; include B; end
class L; prepend C; end
x = L.new
p x.is_a?(A), A === x, x.is_a?(B), x.is_a?(C), x.kind_of?(A)
p L.ancestors.take(5)
p L.include?(A), L < A, L <= B, A > L
p x.a_m
case x when A then puts "when A" else puts "no" end
p [x, 1, "s"].grep(A).size
v = [x, 1].sample
v = x
p v.is_a?(A), A === v

# a module's own prepend comes with it, ahead of the module
module P; end
module D; prepend P; include A; end
class N; prepend D; end
p N.ancestors.take(5), N.new.is_a?(P), N.include?(P)

# a module the class also includes shows twice, as in CRuby
class Q; include A; prepend C; end
p Q.ancestors.take(6)

# two prepends that share a module: it is put in once
module E; include A; end
class R; prepend E; prepend C; end
p R.ancestors.take(6)

# an exception class
class MyErr < StandardError; prepend C; end
begin
  raise MyErr, "boom"
rescue A => e
  p [:rescued, e.class, e.is_a?(B)]
end
p MyErr.ancestors.take(4)

# a subclass sees its superclass's prepends
class L2 < L; end
p L2.new.is_a?(A), L2.ancestors.take(6)

# a prepended module's own prepends come with it, with their includes
module B9; end
module A9; end
module P9; include B9; end
module D9; prepend P9; include A9; end
class N9; prepend D9; end
x9 = N9.new
p x9.is_a?(B9), B9 === x9, N9 < B9, N9.include?(B9), N9 <= P9
case x9 when B9 then puts "B9" else puts "no B9" end
v9 = [x9, 1][0]
p v9.is_a?(B9), N9.ancestors.take(6)
module Q8; end
module P8; prepend Q8; end
module D8; prepend P8; end
class N8; prepend D8; end
p N8.ancestors.take(5), N8.new.is_a?(Q8)
