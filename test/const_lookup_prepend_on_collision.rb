# A module a class prepends comes before the class's own constants and its
# superclass's when a colliding constant name is looked up in the ancestors.
class A; K = 1; end
class B; K = 2; end
module M; K = 3; end
class C < B; prepend M; def self.x = K; end
p C.x
module N; include M; end
class D < A; prepend N; def self.x = K; end
p D.x
class E < B; include M; def self.x = K; end
p E.x
class F < A; def self.x = K; end
p F.x
