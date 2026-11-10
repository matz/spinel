# const_get reads CRuby's lookup order (a module the class includes or
# prepends before its superclass), and a class value raises NameError for a
# class whose chain lacks the constant.
module A; X = :a; end
module P; X = :p; end
class Base; X = :base; end
class K < Base; include A; end
class K2 < Base; prepend P; include A; end
class K3 < K; end
class K4 < Base; include A; X = :own; end
p K.const_get(:X), K2.const_get(:X), K3.const_get(:X), K4.const_get(:X)
p K.const_get("X"), K::X, K2::X, K3::X
module B2; end
module A2; include B2; end
module B2; Y = :b2; end
class J < Base; include A2; end
class Base; Y = :basey; end
p J.const_get(:Y), J::Y
class Own; Z = :own; end
class Other; Y2 = :y; end
class Sub < Own; end
ks = [Own, Other, Sub]
ks.each { |k| p((k.const_get(:Z) rescue $!.message)) }
p((Integer.const_get(:Z) rescue $!.class))
def g(k) = k.const_get(:Z)
p g(Sub), (g(Other) rescue $!.message)
p ks.map { |k| k.const_defined?(:Z) }
