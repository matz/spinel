# A call in the copy an extend makes of a module's method (D.f, M#f's
# body) meets the class method of its name live when it runs: the
# extended V's copy on C first, then C's own def, which replaces it.
module V; def v = 1; end
class C; extend V; end
module M; def f = v; end
class D < C; extend M; end
puts D.f
class C; def self.v = 2; end
puts D.f
