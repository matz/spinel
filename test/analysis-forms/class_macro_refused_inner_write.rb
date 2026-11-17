# A macro whose value is used but not computed here stays in the text and runs
# at run time, writing the state: a later macro reading it is left as written
# (a run-time public_send, refused), not expanded with the state before it.
module Consts
  def prefix
    @kind = :box
    lib_prefix
  end
  def constant(n) = const_set(n, prefix)
  def sized(n) = const_set(n, public_send(["calc", @kind, n.to_s.downcase].compact.join("_")))
end
class Calc
  extend Consts
  def self.lib_prefix = "p"
  def self.calc_size = 1
  def self.calc_box_size = 2
  constant :X
  sized :SIZE
end
p Calc::SIZE
