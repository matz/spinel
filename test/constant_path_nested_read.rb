# Test nested ConstantPath reads (A::B::C, M::C::X).

module A
  module B
    C = 7
  end
end

module M
  class C
    X = 11
  end
end

puts A::B::C
puts ::A::B::C
puts M::C::X
puts ::M::C::X

# Relative path should prefer lexical scope; :: should force root scope.
module RootNS
  module Mid
    LEAF = 31
  end
end

module Lex
  module RootNS
    module Mid
      LEAF = 47
    end
  end

  def self.pick_relative
    RootNS::Mid::LEAF
  end

  def self.pick_root
    ::RootNS::Mid::LEAF
  end
end

puts Lex.pick_relative
puts Lex.pick_root
