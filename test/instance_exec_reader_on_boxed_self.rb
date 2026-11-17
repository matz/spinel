# A bare reader in a block instance_exec runs on a value that may be one
# of several classes reads the receiver's own slot: each class's reader
# answers, and the value's class set holds what each slot holds.
class A
  def hi = :a
end
class B
  def hi = :b
end
class Host
  attr_reader :v
  def initialize = @v = A.new
  def run(g) = g.instance_exec { v }
  def run_eval(g) = g.instance_eval { v.hi }
  def run_blk(g, &b) = g.instance_exec(&b)
end
class G
  attr_reader :v
  def initialize = @v = B.new
end
p Host.new.run(G.new).hi
p Host.new.run(Host.new).hi
p Host.new.run_eval(G.new)
p Host.new.run_eval(Host.new)
h = Host.new
p h.run_blk(G.new) { v }.hi
p h.run_blk(Host.new) { v }.hi
