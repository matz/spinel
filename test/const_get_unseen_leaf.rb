# A builtin class or module does not see a constant that only a program
# class holds: R.const_get(:X) is CRuby's NameError, named R::X.
class A; X = 1; end
module M; Y = :m; end
class B; include M; end
def err(e) = [e.class, e.message]
p((Integer.const_get(:X) rescue err($!)))
p((String.const_get(:X) rescue err($!)))
p((Comparable.const_get(:X) rescue err($!)))
p((Integer.const_get(:Zz) rescue err($!)))
p A.const_get(:X)
p B.const_get(:Y)
p((Integer.const_get(:Y) rescue err($!)))
Z = 3
p Integer.const_get(:Z)
p Comparable.const_get(:Z)
p((Kernel.const_get(:Zz) rescue err($!)))
p((BasicObject.const_get(:Zz) rescue err($!)))
p((Object.const_get(:Zz) rescue err($!)))
# a top-level include mixes N's constants into Object, which all receivers see
module N; W = 5; end
include N
p Integer.const_get(:W)
p Comparable.const_get(:W)
p A.const_get(:W)
