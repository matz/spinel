# A `class << self` attr_writer and a method of a module the class extends
# name the same @ivar of the class object. The accessor's slot is the
# class-level ivar's where a class method names it, which was asked before
# the extended module's methods were copied in as class methods: the writer
# kept a slot of its own, and the module's method read the old value.
module Consts
  def kind(k = nil)
    return @k if k.nil?
    @k = k
  end
end
class C
  extend Consts
  class << self
    attr_writer :k
  end
  kind :box
  self.k = :nonce
  p kind
end
p C.kind
class D
  class << self
    attr_writer :k
    def k = @k
  end
  def self.set = @k = :a
  set
  self.k = :b
  p k
end
