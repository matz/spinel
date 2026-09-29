# A macro called inside a case (or a block, a loop) the walk does not follow
# writes what it does not see: a later macro reading the state is left as
# written (a run-time public_send, refused), not expanded with the old value.
module Consts
  def kind(k) = @kind = k
  def constant(c)
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.calc_box_size = 2
  case ARGV.size
  when 0 then kind :box
  end
  constant :SIZE
end
p Calc::SIZE
