# instance_variable_set under a guard may not happen: a macro reading the
# state afterwards is left as written (a run-time public_send, refused).
module Consts
  def constant(c)
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.calc_box_size = 2
  instance_variable_set(:@kind, :box) if ARGV.empty?
  constant :SIZE
end
p Calc::SIZE
