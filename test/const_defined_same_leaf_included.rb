# A constant two modules own, seen through an include: const_defined?
# on the class, its subclass and a BasicObject subclass.
module M; Z = 5; end
module N; Z = 6; end
class K; include M; end
class L < K; end
class Q; end
p L.const_defined?(:Z), L.const_defined?(:Z, false), Q.const_defined?(:Z)

class BB < BasicObject; include ::M; end
class BC < BasicObject; end
p BB.const_defined?(:Z), BC.const_defined?(:Z)

