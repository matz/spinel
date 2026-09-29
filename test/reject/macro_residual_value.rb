# A macro whose value is a call the evaluator does not make has no value at
# compile time: a name built from it is not computed (a run-time public_send,
# refused), not built with nil in its place.
module Consts
  def prefix = lib_prefix
  def constant(c)
    const_set(c, public_send(["calc", prefix, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.lib_prefix = "box"
  def self.calc_size = 1
  def self.calc_box_size = 2
  constant :SIZE
end
p Calc::SIZE
