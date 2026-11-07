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
