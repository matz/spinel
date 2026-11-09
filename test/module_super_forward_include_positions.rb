# Where the module stands decides what its `def m(...) = super` reaches: a
# prepend reaches the class's own def, a chain of modules or a diamond reaches
# the superclass's, a constant alias or a path names the superclass, and a
# Data or Hash-rooted superclass has its own def ahead of the builtin one. In
# each the module's keywords stay keywords.

module P1
  module Wrap
    def check(...) = super
  end
  class C
    prepend Wrap
    def check(a, b = 2, k: 3) = [:c, a, b, k]
  end
end
p P1::C.new.check(1, k: 5)
p P1::C.new.check(1, 9)

module P2
  class Base
    def check(a, b = 2, k: 3) = [a, b, k]
  end
  module Wrap
    def check(...) = super
  end
  class C < Base
    prepend Wrap
  end
end
p P2::C.new.check(1, k: 5)
p P2::C.new.check(1, 9)

module P3
  class Base
    def check(a, b = 2, k: 3) = [a, b, k]
  end
  module Wrap
    def check(...) = super
  end
  module A
    include Wrap
  end
  module B
    include Wrap
  end
  class C < Base
    include A
    include B
  end
end
p P3::C.new.check(1, k: 5)
p P3::C.new.check(1, 9)

module P4
  class Base
    def check(a, b = 2, k: 3) = [a, b, k]
  end
  module M0
    def check(...) = super
  end
  module M1
    include M0
  end
  module M2
    include M1
  end
  class C < Base
    include M2
  end
end
p P4::C.new.check(1, k: 5)
p P4::C.new.check(1, 9)

module P5
  class Base
    def check(a, b = 2, k: 3) = [a, b, k]
  end
  module Wrap
    def check(...) = super
  end
  B2 = Base
  class C < B2
    include Wrap
  end
end
p P5::C.new.check(1, k: 5)
p P5::C.new.check(1, 9)

module P6
  module Wrap
    def check(...) = super
  end
  D = Data.define(:x) do
    def check(a, b = 2, k: 3) = [a, b, k]
  end
  class E2 < D
    include Wrap
  end
end
p P6::E2.new(x: 1).check(1, k: 5)
p P6::E2.new(x: 1).check(1, 9)

module P7
  class H < Hash
    def min_by(a, b = 2, k: 3) = [a, b, k]
  end
  module Wrap
    def min_by(...) = super
  end
  class H2 < H; end
  class H2
    include Wrap
  end
end
p P7::H2.new.min_by(1, k: 5)
p P7::H2.new.min_by(1, 9)

module P8
  class Left
    def check(a, b = 2, k: 3) = [:left, a, b, k]
  end
  class Right
    def check(a, b = 2, k: 3) = [:right, a, b, k]
  end
  module Wrap
    def check(...) = super
  end
  class L < Left
    include Wrap
  end
  class R < Right
    include Wrap
  end
end
p P8::L.new.check(1, k: 5)
p P8::R.new.check(1, 9)
