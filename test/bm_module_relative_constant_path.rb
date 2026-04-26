# Test: relative ConstantPath class/module definitions inside a module.

module M
  class A
  end

  class A::B
    def value
      10
    end
  end

  module N
  end

  module N::P
    class C
      def value
        30
      end
    end
  end
end

puts M::A::B.new.value
puts M::N::P::C.new.value
