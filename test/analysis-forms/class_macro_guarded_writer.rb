# A macro that writes the state, under a modifier: it may not run, so a later
# macro reading the state is left as written (a run-time public_send, refused),
# not expanded with the write taken as made.
module Consts
  def kind(k)
    @kind = k
  end
  def constant(c)
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.calc_box_size = 2
  kind :box if ARGV.empty?
  constant :SIZE
end
p Calc::SIZE
