# An index op-write through a boxed value types only the Structs it may hold
# `buf[i] += v` (or `||=`) on a value of no static type is written out during
# inference as a read and a store through a held copy of the receiver. The
# class facts were computed before that, so the new `[]=` had none and was
# taken as possibly any Struct's: its boxed value widened the members of
# every Struct in the program. The written-out reads now hold the
# receiver's fact, so Point's members stay Integers; Pair's, which a box
# that may hold a Pair stores a Float into, still widen, and so do Cell's,
# which a box that may hold a Cell `||=`s a String into.

Point = Struct.new(:x, :y)
Pair = Struct.new(:a, :b)
Cell = Struct.new(:v, :w)

def bump(buf, i)
  buf[i] += 1
end

def fill(buf, i)
  buf[i] ||= 7
end

def tag(buf, i)
  buf[i] ||= "s"
end

def scale(box, k)
  box[k] += 0.5
end

def mark(box, k)
  box[k] ||= "s"
end

def fill_box(box, i)
  box[i] ||= 7
end

bufs = {}
[1, 2].each { |k| bufs[k] = Array.new(4, 0) }
bump(bufs[1], 1)
fill(bufs[2], 3)
p bufs

tags = {}
[1, 2].each { |k| tags[k] = Array.new(2) }
tag(tags[1], 0)
p tags

pair = Pair.new(1, 2)
scale([pair, [1.0]][ARGV.size], :a)
scale([[1.0], pair][ARGV.size], 0)
p pair

cell = Cell.new(nil, 3)
p mark([cell, [1.0]][ARGV.size], :v)
p fill_box([Array.new(4), [1.0]][ARGV.size], 3)
p cell, cell.w + 1

pts = Array.new(10) { |i| Point.new(i, i * 2) }
p pts.sum { |pt| pt.x * pt.y }
