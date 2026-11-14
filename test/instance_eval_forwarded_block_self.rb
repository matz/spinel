# A block a method hands on to another receiver's instance_eval/exec
# (`def build(&blk) = @root.instance_eval(&blk)`) runs with that receiver
# as self: its self is not the calling method's.
class Node; def initialize = (@kids = []); end
class Leaf; def initialize = (@kids = nil); end
class Builder
  def initialize = (@root = Node.new)
  def build(&blk) = @root.instance_eval(&blk)
  def go = build { [self, Leaf.new].map { |o| o.instance_eval { @kids ||= [:x] } } }
end
p Builder.new.go

class A; def initialize = (@w = 1); def w = @w; end
class B; def initialize = (@w = 2); def w = @w; end
class W
  def initialize = (@q = 0)
  def run(a, &blk) = a.instance_eval(&blk)
  def run_exec(a, &blk) = a.instance_exec(&blk)
  def go(a) = run(a) { [self, B.new].map { |o| o.instance_eval { @w ||= 7 } } }
  def go_exec(a) = run_exec(a) { [self, B.new].map { |o| o.instance_eval { @w ||= 7 } } }
  def set(a) = run(a) { [self, B.new].each { |o| o.instance_eval { @w = 5 } }.map(&:w) }
end
p W.new.go(A.new)
p W.new.go_exec(A.new)
p W.new.set(A.new)
