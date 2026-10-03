# alias_method takes its names as Strings too: the method it makes runs
# the block as a block, so the block's `next` stays a `next`.

class Inst
  def initialize = @on = true
  def call_it(name, &b) = b.call
  alias_method "define_method", "call_it"

  def run
    v = define_method(:a) { next 1 if @on; 2 }
    [v, :after]
  end
end
p Inst.new.run
