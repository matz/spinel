# The text pass refuses this call (test/reject/macro_module_attr_writer.rb);
# the class-body macro form runs the macro at the call's point under the
# load graph, as CRuby runs it.
module Consts
  attr_writer :k
  def kind(k = nil)
    return @k if k.nil?
    @k = k
  end
  def constant(c) = const_set(c, public_send(["calc", kind, c.to_s.downcase].join("_")))
end
class C
  extend Consts
  def self.calc_box_size = 32
  def self.calc_nonce_size = 24
  kind :box
  self.k = :nonce
  constant :SIZE
end
p C::SIZE
