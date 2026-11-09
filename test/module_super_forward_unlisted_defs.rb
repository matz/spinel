# A module's `def zork(...) = super` reaches the def of a class between the
# includer and a farther class whose def takes fewer channels. The def stands
# in a form that is no statement of the class body: a begin body, a class_eval
# block in the body or at the top level, a hook's class_eval, an alias or
# alias_method, a singleton class_eval reached through extend. The keyword the
# caller passes stays a keyword on the way to the nearer def. Another class's
# `zork` takes the keyword too. N7 is a hook's class_eval def over a builtin's
# method of the same name (StopIteration#result).

class Other
  def zork(a, b = 2, k: 3) = [:other, a, b, k]
end

module N1
  class Root; def zork(a) = [:root, a]; end
  module Wrap; def zork(...) = super; end
  class Mid < Root
    begin
      def zork(a, b = 2, k: 3) = [:mid, a, b, k]
    end
  end
  class C < Mid; include Wrap; end
end
p N1::C.new.zork(1, k: 5)
p N1::C.new.zork(1, 9)

module N2
  class Root; def zork(a) = [:root, a]; end
  module Wrap; def zork(...) = super; end
  class Mid < Root; end
  Mid.class_eval do
    def zork(a, b = 2, k: 3) = [:mid, a, b, k]
  end
  class C < Mid; include Wrap; end
end
p N2::C.new.zork(1, k: 5)
p N2::C.new.zork(1, 9)

module N3
  class Root; def zork(a) = [:root, a]; end
  module Wrap; def zork(...) = super; end
  module Ext
    def self.included(base)
      base.class_eval do
        def zork(a, b = 2, k: 3) = [:mid, a, b, k]
      end
    end
  end
  class Mid < Root
    include Ext
  end
  class C < Mid; include Wrap; end
end
p N3::C.new.zork(1, k: 5)
p N3::C.new.zork(1, 9)

module N4
  class Root; def zork(a) = [:root, a]; end
  module Wrap; def zork(...) = super; end
  class Mid < Root
    def real(a, b = 2, k: 3) = [:mid, a, b, k]
    alias zork real
  end
  class C < Mid; include Wrap; end
end
p N4::C.new.zork(1, k: 5)

module N5
  class Root; def zork(a) = [:root, a]; end
  module Wrap; def zork(...) = super; end
  class Mid < Root
    def real(a, b = 2, k: 3) = [:mid, a, b, k]
    alias_method :zork, :real
  end
  class C < Mid; include Wrap; end
end
p N5::C.new.zork(1, k: 5)

module N6
  class Root
    def self.zork(a) = [:root, a]
  end
  module CM; def zork(...) = super; end
  class Mid < Root; end
  Mid.singleton_class.class_eval do
    def zork(a, b = 2, k: 3) = [:mid, a, b, k]
  end
  class C < Mid; extend CM; end
end
p N6::C.zork(1, k: 5)
p Other.new.zork(1)

module N7
  module Wrap; def result(...) = super; end
  module Ext
    def self.included(base)
      base.class_eval do
        def result(a, b = 2, k: 3) = [:mid, a, b, k]
      end
    end
  end
  class Mid < StopIteration
    include Ext
  end
  class C < Mid; include Wrap; end
end
p N7::C.new.result(1, k: 5)
p N7::C.new.result(1, 9)
