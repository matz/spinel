# An empty `{}` has no elements to type it: inference gives the literal node
# whatever the reads elsewhere suggest, and the write it goes into lends it
# the slot's variant (emit_empty_literal_as). The inner write of a chain,
# `@cache = cache = {}`, is an expression, and its emitter built an empty
# `[]` at the local's slot but an empty `{}` at the literal's own variant: a
# String-keyed Hash went into the poly-keyed slot a large application
# program's relation delegate cache has (its keys are classes).
class Relation; end
class Proxy; end
class Base
  class << self
    def relation_delegate_class(klass)
      @relation_delegate_cache[klass]
    end
    def initialize_relation_delegate_cache
      @relation_delegate_cache = cache = {}
      [Relation, Proxy].each_with_index { |klass, i| cache[klass] = "#{klass.name}Delegate#{i}" }
      @relation_delegate_cache.freeze
    end
    def names_by_class
      @names = names = {}
      names[Relation] = :rel if ARGV.size < 9
      names
    end
  end
  initialize_relation_delegate_cache
end
p Base.relation_delegate_class(Relation), Base.relation_delegate_class(Proxy), Base.relation_delegate_class(String)
p Base.names_by_class, Base.names_by_class.equal?(Base.instance_variable_get(:@names))

# the array twin, and a chain through two locals
def chains
  a = b = {}
  b[:k] = 1
  @xs = xs = []
  xs << 2
  [a, b.equal?(a), xs, @xs.equal?(xs)]
end
p chains
