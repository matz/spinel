# `include M` in a `class << self` body after other code: the load record
# takes it as an extend of the class at its point (#10790).
module M; def hi = "M"; end
class K
  class << self
    p respond_to?(:hi)
    include M
  end
end
p K.hi

module Greet; def greet = "hello from #{name}"; end
class Svc
  class << self
    def boot = :booted
    include Greet
  end
end
p Svc.boot, Svc.greet
