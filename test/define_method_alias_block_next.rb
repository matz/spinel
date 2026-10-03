# A method named define_method or define_singleton_method that an alias
# makes runs its block as a block, so the block's `next` stays a `next`.

class Inst
  def initialize = @on = true
  def call_it(name, &b) = b.call
  alias define_method call_it

  def run
    v = define_method(:a) { next 1 if @on; 2 }
    [v, :after]
  end
end
p Inst.new.run

class Sub < Inst
  def go
    v = define_method(:b) { next if @on; 2 }
    [v, :sub]
  end
end
p Sub.new.go

$on = true
class Cls
  def self.call_it(name, &b) = b.call
  class << self
    alias define_method call_it
    alias_method :define_singleton_method, :call_it
  end

  r = define_method(:m) { next 5 if $on; 6 }
  p r
  s = define_singleton_method(:n) { next 7 if $on; 8 }
  p s

  def self.go
    v = define_method(:o) { next 9 if $on; 10 }
    [v, :cls]
  end
end
p Cls.go
