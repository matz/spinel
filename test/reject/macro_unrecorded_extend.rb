# A macro reaching the class by an extend not recorded here (an extended
# hook's `base.extend`): its call writes what is not followed, so a later
# macro reading the state is left as written (a run-time public_send, refused).
module Kinds
  def kind(k) = (@kind = k)
end
module Consts
  def self.extended(base) = base.extend(Kinds)
  def constant(c)
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.calc_box_size = 2
  kind :box
  constant :SIZE
end
p Calc::SIZE
