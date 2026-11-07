# `alias include ci` (and prepend, extend) on a class's singleton: the
# mixin call runs the method the alias took, which mixes nothing in.
module M; def hi = :m; end
module N; def ho = :n; end
class K
  class << self
    def ci(m) = puts("ci #{m}")
    alias include ci
  end
  include M
end
p K.new.respond_to?(:hi)
class K2
  def self.ci(m) = puts("k2 #{m}")
  singleton_class.alias_method :include, :ci
  include M
end
p K2.new.respond_to?(:hi)
class Base
  class << self
    def ci(m) = puts("base #{m}")
    alias prepend ci
  end
end
class Sub < Base
  prepend N
end
p Sub.new.respond_to?(:ho)
class Two
  class << self
    def a1(m) = puts("a1 #{m}")
    alias a2 a1
    alias include a2
  end
  include M
end
p Two.new.respond_to?(:hi)
class Late
  class << self
    def ci(m) = puts("old #{m}")
    alias include ci
    def ci(m) = puts("new #{m}")
  end
  include M
end
class Ext2; extend M; end
p Ext2.respond_to?(:hi)
class Ex3
  class << self
    def ce(m) = puts("ce #{m}")
    alias extend ce
  end
  extend N
end
p Ex3.respond_to?(:ho)
