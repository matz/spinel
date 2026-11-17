# A macro's state written in branches that disagree is decided at run time:
# the macro reading it is left as written (a run-time public_send, refused),
# not expanded with one branch's value.
module Consts
  def kind(k = nil)
    return @kind if k.nil?
    @kind = k
  end
  def constant(c)
    const_set(c, public_send(["calc", kind, c.to_s.downcase].join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_a_size = 1
  def self.calc_b_size = 2
  if ARGV.empty?
    kind :a
  else
    kind :b
  end
  constant :SIZE
end
p Calc::SIZE
