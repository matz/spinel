# A private method of a reopened Object, called on a class value: Class's
# own public methods (superclass, allocate) answer a class first, and
# Module's (ancestors) answer a class or a module. A module value has no
# superclass, so it gets the private-method error, as in CRuby.
class Object
  private def superclass = "obj"
  private def ancestors = "obj"
  private def allocate = "obj"
end
class K; end
class L < K
  def self.up = superclass
end
module M; end
class E < StandardError; end
p K.superclass, L.superclass, E.superclass
p L.up
p K.allocate.class
p K.ancestors.first(2), M.ancestors
p((M.superclass rescue $!.message))
x = K
p x.superclass
[K, L, M, 3, Integer, Comparable].each { |c| p((c.superclass rescue $!.message)) }
[K, M, 3].each { |c| p((c.ancestors.first rescue $!.message)) }
[K, M].each { |c| p((c.allocate.class rescue $!.message)) }
