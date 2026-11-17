# An Array of one class stays narrowed (sp_PtrArray) where it is read by
# max_by, min_by (with and without a count), max or min with a comparator
# block, or sample. max_by and min_by's definitions read the receiver only
# through each, even on the count's path, and the other three have emitters
# for an Array of one class. Each Array here is read by one of them and by an
# element read; infer-test checks each stays an sp_PtrArray.
class Node
  attr_reader :v
  def initialize(v) = @v = v
  def score = @v * 1.5
end

def fill(n) = Array.new(n) { |i| Node.new((i * 37) % n) }

by_max = []
fill(9).each { |x| by_max << x }
p by_max.max_by(&:score).v, by_max.max_by { |x| -x.v }.v, by_max[3].v
p by_max.max_by(3, &:score).map(&:v)

by_min = []
fill(9).each { |x| by_min << x }
p by_min.min_by(&:score).v, by_min.min_by(2) { |x| -x.v }.map(&:v), by_min[3].v

cmp_max = []
fill(7).each { |x| cmp_max << x }
p cmp_max.max { |a, b| a.v <=> b.v }.v, cmp_max.max { |a, b| (a.v % 3) <=> (b.v % 3) }.v, cmp_max[2].v

cmp_min = []
fill(7).each { |x| cmp_min << x }
p cmp_min.min { |a, b| a.score <=> b.score }.v, cmp_min.min { |a, b| b.v - a.v }.v, cmp_min[2].v

pool = []
fill(5).each { |x| pool << x }
p pool.include?(pool.sample), pool.sample.class, pool[1].v

# none: nil from each
empty = []
fill(1).each { |x| empty << x }
empty.clear
p empty.max_by(&:score), empty.min_by(&:score), empty.max { |a, b| a.v <=> b.v },
  empty.min { |a, b| a.v <=> b.v }, empty.sample, empty.max_by(2, &:score)

# one: itself from each
one = []
fill(1).each { |x| one << x }
p one.sample.v, one.max { |a, b| raise "not called" }.v, one.min_by(&:score).v

# a comparator that answers nil raises
bad = []
fill(3).each { |x| bad << x }
begin
  bad.max { |a, b| nil }
rescue ArgumentError => e
  puts "ArgumentError: #{e.message}"
end
# the walk reads the live length, as CRuby's does: a comparator that takes
# elements out (with a collection in between) or pushes new ones
shrink = []
fill(5).each { |x| shrink << x }
m = shrink.max { |a, b| shrink.delete(b); GC.start; Array.new(20) { "x" * 40 }; a.v <=> b.v }
p m.v, shrink.size
grow = []
fill(4).each { |x| grow << x }
m = grow.min { |a, b| grow << Node.new(-1) if grow.size < 6; a.v <=> b.v }
p m.v, grow.size

# a comparator the loop does not lower still compiles (unreached here)
bad.max { |a, b| "x" } if ARGV.size > 5
p bad[0].v

# min_by(n) and max_by(n) take each key during the walk, as CRuby does: a
# key block that changes the receiver changes what the walk sees
a = [3, 1, 2]
p a.min_by(1) { |x| a.clear; x }
b = [3, 1, 2]
p b.max_by(1) { |x| b.clear; x }
c = [3, 1, 2]
p c.min_by(2) { |x| c << 0 if c.size < 5; x }
d = [3, 1, 2]
p d.max_by(2) { |x| d << 9 if d.size < 5; x }
# ... once per element, in the walk's order
log = []
p [3, 1, 2].max_by(2) { |x| log << x; x }, log

# with a count, the walk keeps a bounded buffer as CRuby's nmin_run does: a
# count of 0 runs no walk, the buffer is cut to the best n whenever it fills
# (4n entries), and a key that cannot be ordered raises where CRuby does
calls = 0
p [5, 6].min_by(0) { |x| calls += 1; x }, [5, 6].max_by(0) { |x| calls += 1; x }, calls
big = Array.new(50) { |i| (i * 37) % 50 }
p big.min_by(3) { |x| x }, big.max_by(4) { |x| x * 2 }, big.min_by(7) { |x| -x }
pool2 = []
fill(40).each { |x| pool2 << x }
p pool2.max_by(5, &:score).map(&:v), pool2.min_by(2) { |x| x.v }.map(&:v)
[[[1, 2, 3, 4], 2, :min], [[1, 2, 3, 4], 1, :max], [["a", 1], 1, :min], [[1, 2, 3], 2, :min], [[1, 2, 3], 2, :max]].each do |arr, n, which|
  begin
    if which == :min
      arr.min_by(n) { |x| x == 4 ? "s" : x.is_a?(String) ? x : x == 2 && arr.size == 3 ? nil : x }
    else
      arr.max_by(n) { |x| x == 3 && arr.size == 4 ? :k : x == 2 && arr.size == 3 ? nil : x }
    end
  rescue ArgumentError => e
    p e.message
  end
end

# more than 16 kept: the buffer is cut and the kept ones are sorted whole
# (a key whose `<=>` answers the most negative Integer: max_by_n_cut_sign.rb)
wide = Array.new(80) { |i| (i * 37) % 80 }
p wide.min_by(17) { |x| x }, wide.max_by(17) { |x| x * 3 }
