# An empty elsif leaves the macro's state as it was: branches that then
# disagree leave the macro reading it as written (a run-time public_send,
# refused).
module Consts
  def kind(k = nil)
    return @kind if k.nil?
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
  def self.calc_other_size = 3
  if ARGV.empty?
    kind :box
  elsif ARGV.size > 1
  else
    kind :box
  end
  constant :SIZE
end
p Calc::SIZE
