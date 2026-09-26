# An anonymous `&` forwarded into a method that yields, reached from a
# method that keeps its own named block: the forwarder was inlined with no
# literal block to yield to, and the helper raised LocalJumpError.
class H
  def helper = yield(5)
  def run(&) = helper(&)
  def twice(&) = [helper(&), run(&)]
end
class O
  def go(&blk)
    @kept = blk
    H.new.run(&blk)
  end

  def both(&blk)
    @kept = blk
    H.new.twice(&blk)
  end
end
p O.new.go { |v| v * 3 }
p O.new.both { |v| v + 1 }
