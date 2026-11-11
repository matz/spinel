# A mixin made through send by a String name, or through a send of a send,
# is the call of include or prepend it names: the module's methods are the
# class's from there, as for a plain C.include M. The second class includes
# the module after a first call of its method.
module M; def m = :m; end
module P; def pp = :p; def m = [:pm, super]; end
module N; def nn = :n; end
class C; def own = :own; end
C.send("include", M)
C.send(:send, :prepend, P)
C.__send__(:public_send, "include", N)
o = C.new
p [o.m, o.pp, o.nn, o.own, C.ancestors.take(4), C.include?(N), C.ancestors.index(P) < C.ancestors.index(C)]

module Q; def q = :q; end
class D; end
def f(o) = (begin; o.q; rescue => e; e.class; end)
p f(D.new)
D.send(:send, :include, Q)
p f(D.new)
