# frozen_string_literal: true
# A block-form scan returns the object it walked, even if the block rebinds
# its source variable or the object belongs to a String subclass.
def walk_rebound(s)
  s.scan(/./) { s = +"other" }
end
class WalkReceiver
  def initialize(s) = (@s = s)
  def walk
    @s.scan(/./) { @s = +"other" }
  end
end
s = +"ab"
p walk_rebound(s).equal?(s), WalkReceiver.new(s).walk.equal?(s)

class ScanMark < String
end
class ScanChild < ScanMark
end
def walk_subclass(s)
  s.scan(/./) { }
end
m = ScanChild.new("ab")
r = walk_subclass(m)
p r.equal?(m), r.class
r << "c"
p m
p m.scan(/(.)/) { |part| part }.equal?(m)
p m.scan("b") { }.equal?(m)
p m.scan(/./) { break :stopped }
p m.scan(/./) { next :ignored }.equal?(m)

def maybe_walk(s)
  s&.scan(/./) { }
end
p maybe_walk(nil)
p maybe_walk(s).equal?(s)

# Value positions retain the same receiver as method tails.
s = +"ab"
original = s
r = s.scan(/(.)/) { s = +"replacement" }
p r.equal?(original)
s = original
r = s.scan(/./) { s = +"replacement" }
p r.equal?(original)

# An argument can rebind the source before the first block invocation.
s = original
r = s.scan((s = +"replacement"; /./)) { }
p r.equal?(original)

class ScanGrandchild < ScanChild
end
def walk_grandchild(s, stop)
  s.scan(/./) { break :early if stop }
end
g = ScanGrandchild.new("g")
p walk_grandchild(g, false).equal?(g)
p walk_grandchild(g, true)

class ScanOverride < String
  def scan(pattern)
    yield "own"
    :own_result
  end
end
p ScanOverride.new("x").scan(/./) { |v| puts v }
