# An attr_writer on the singleton class writes the macro state through
# `self.k = v`: the macro reading it is left as written (a run-time public_send over the class's calc_* methods).
module Consts
  def kind(k = nil)
    return @k if k.nil?
    @k = k
  end
  def constant(c) = const_set(c, public_send(["calc", kind, c.to_s.downcase].join("_")))
end
class C
  extend Consts
  class << self
    attr_writer :k
  end
  def self.calc_box_size = 32
  def self.calc_nonce_size = 24
  kind :box
  self.k = :nonce
  constant :SIZE
end
p C::SIZE
