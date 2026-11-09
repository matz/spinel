# A module method `def m(...) = super` hands its caller's keywords on as
# keywords when every position the module has reaches a def of the program's,
# whatever statement mixed the module in: a modifier or a begin around the
# include, a nested namespace, `K.include M` and `K.public_send(:include, M)`
# from outside the class, a reopening, several modules in one include, a
# module that includes it in a hook, a Class.new builder, an include after
# the first call. Each case has a base class defining `check` with an
# optional and a keyword, a name some builtin answers too.

module S1
  class Base
    def check(a, b = 2, k: 3) = [a, b, k]
  end
  module Wrap
    def check(...) = super
  end
  class C < Base
    include Wrap unless false
  end
end
p S1::C.new.check(1, k: 5)
p S1::C.new.check(1, 9)

module S2
  class Base
    def check(a, b = 2, k: 3) = [a, b, k]
  end
  module Wrap
    def check(...) = super
  end
  class C < Base
    begin
      include Wrap
    end
  end
end
p S2::C.new.check(1, k: 5)
p S2::C.new.check(1, 9)

class Base
  def check(a, b = 2, k: 3) = [a, b, k]
end
module NS
  module Wrap
    def check(...) = super
  end
  class C < ::Base
    include Wrap
  end
  class D < ::Base; end
end
NS::D.include(NS::Wrap)
p NS::C.new.check(1, k: 5)
p NS::D.new.check(1, k: 5)
p NS::D.new.check(1, 9)

module S4
  class Base
    def check(a, b = 2, k: 3) = [a, b, k]
  end
  module Wrap
    def check(...) = super
  end
  class C < Base; end
end
S4::C.public_send(:include, S4::Wrap)
p S4::C.new.check(1, k: 5)
p S4::C.new.check(1, 9)

module S5
  class Base
    def check(a, b = 2, k: 3) = [a, b, k]
  end
  module Wrap
    def check(...) = super
  end
  class C < Base; end
  class C
    include Wrap
  end
end
p S5::C.new.check(1, k: 5)
p S5::C.new.check(1, 9)

module S6
  module Wrap
    def check(...) = super
  end
  module Other
    def check(a, b = 2, k: 3) = [:other, a, b, k]
  end
  class Base
    def check(*) = :base
  end
  class C < Base
    include Wrap, Other
  end
end
p S6::C.new.check(1, k: 5)
p S6::C.new.check(1, 9)

module S7
  class Base
    def check(a, b = 2, k: 3) = [a, b, k]
  end
  module Wrap
    def check(...) = super
  end
  module Hooky
    def self.included(b) = b.include(Wrap)
  end
  class C < Base
    include Hooky
  end
end
p S7::C.new.check(1, k: 5)
p S7::C.new.check(1, 9)

module S8
  class Base
    def check(a, b = 2, k: 3) = [a, b, k]
  end
  module Wrap
    def check(...) = super
  end
  K = Class.new(Base) { include Wrap }
end
p S8::K.new.check(1, k: 5)
p S8::K.new.check(1, 9)

module S9
  class Base
    def check(a, b = 2, k: 3) = [a, b, k]
  end
  module Wrap
    def check(...) = super
  end
  class C < Base; end
end
p S9::C.new.check(1, k: 5)
S9::C.include(S9::Wrap)
p S9::C.new.check(1, k: 5)
p S9::C.new.check(1, 9)
