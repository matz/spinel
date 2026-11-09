# The def a module's `super` reaches may stand ahead of a builtin's method of
# the same name (StopIteration#result): under a literal condition, from
# define_method, or private. The module's keywords stay keywords. Where the
# super reaches the builtin itself (Object#to_s, BasicObject#initialize,
# Comparable#clamp) the module keeps forwarding what the callers pass.

module D1
  class Base < StopIteration
    if true
      def result(a, b = 2, k: 3) = [a, b, k]
    end
  end
  module Wrap
    def result(...) = super
  end
  class C < Base
    include Wrap
  end
end
p D1::C.new.result(1, k: 5)
p D1::C.new.result(1, 9)

module D2
  class Base < StopIteration
    define_method(:result) { |a, b = 2, k: 3| [a, b, k] }
  end
  module Wrap
    def result(...) = super
  end
  class C < Base
    include Wrap
  end
end
p D2::C.new.result(1, k: 5)
p D2::C.new.result(1, 9)

module D3
  class Base < StopIteration
    private def result(a, b = 2, k: 3) = [a, b, k]
    public
    def go(*a, **k) = result(*a, **k)
  end
  module Wrap
    def result(...) = super
  end
  class C < Base
    include Wrap
  end
end
p D3::C.new.go(1, k: 5)
p D3::C.new.go(1, 9)

module D4
  module Wrap
    def to_s(...) = super
  end
  class C
    include Wrap
  end
end
p D4::C.new.to_s.start_with?("#<D4::C")

module D5
  module Wrap
    def initialize(...)
      super
      @ok = true
    end
    attr_reader :ok
  end
  class C
    include Wrap
  end
end
p D5::C.new.ok

module D6
  module Wrap
    include Comparable
    def clamp(...) = super
  end
  class V
    include Wrap
    attr_reader :n
    def initialize(n) = @n = n
    def <=>(o) = n <=> o.n
  end
end
p D6::V.new(5).clamp(D6::V.new(1), D6::V.new(3)).n

module D7
  class Base
    def initialize(a, b = 2, k: 3) = @v = [a, b, k]
    attr_reader :v
  end
  module Wrap
    def initialize(...) = super
  end
  class C < Base
    include Wrap
  end
end
p D7::C.new(1, k: 5).v
p D7::C.new(1, 9).v
