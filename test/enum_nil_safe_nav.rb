# A `&.` call that a desugar lowers through an interposed step (to_a, map,
# sort, each, =~) answers nil for a nil receiver, as CRuby does,
# instead of running the step on nil.
class Bag
  include Enumerable
  def initialize(*v) = (@v = v)
  def each(&b) = @v.each(&b)
end
class Sub < Bag; end
class Mt
  def =~(o) = o == 1 ? 0 : nil
end
def bag(f) = f ? Bag.new(3, 1, 2) : nil
def sub(f) = f ? Sub.new(3, 1, 2) : nil
def ints(f) = f ? [3, 1, 2] : nil
def mixed(f) = f ? [3, "a", 2] : nil
def str(f) = f ? "abc" : nil
def mt(f) = f ? Mt.new : nil
[false, true].each do |f|
  p bag(f)&.map { |x| x * 2 }
  p bag(f)&.select { |x| x > 1 }
  p bag(f)&.first
  p sub(f)&.to_h { |x| [x, x] }
  p sub(f)&.sum { |x| x * 2 }
  p ints(f)&.min(2) { |a, b| a <=> b }
  p ints(f)&.max(2) { |a, b| a <=> b }
  p ints(f)&.to_h { |v| [v, v] }
  p mixed(f)&.to_h { |v| [v, v] }
  p ints(f)&.each_with_index&.reverse_each { |v, i| v }
  s = str(f)
  p s&.each_char&.with_index { |ch, i| ch * i }
  p mt(f)&.!~(1)
end
class Sub
  def to_h(*) = [:own]
  def sum(*) = [:own]
end
