# A block parameter whose only value is an argument that never arrives: the
# call that computes it raises first (a method whose body is a raise, as a
# pruned method's is). The value binds nothing, so it says nothing of the
# parameter's type, and the block compiles; where another site hands a
# value, the parameter is that value's type.
def bottom = raise(NotImplementedError, "pruned")

def feed(&blk) = blk.call(bottom)

def feed_yield = yield(bottom)

def feed_both(n, &blk)
  blk.call(n) if n
  blk.call(bottom)
end

x = 1
begin
  feed { |snapshot| x = snapshot }
rescue NotImplementedError => e
  p e.message
end
p x

begin
  feed_yield { |snapshot| x = snapshot }
rescue NotImplementedError => e
  p e.message
end
p x

begin
  feed_both(7) { |n| x = n + 1 }
rescue NotImplementedError => e
  p e.message
end
p x

# the block is handed on and never called at all
module Fmt
  def self.build(builtins, &publish) = builtins.dup
end
module Formats
  @list = { db: "%Y-%m-%d" }.freeze
  @shared = @list
  LIST = Fmt.build(@list) do |snapshot|
    @shared = snapshot
  end
  def self.lookup(f) = LIST[f]
end
p Formats.lookup(:db)
