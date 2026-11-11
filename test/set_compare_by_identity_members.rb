# An identity Set finds its members by equal?, as CRuby's does, whatever
# their kind: a String literal is one object per content (frozen string
# literals), a String made afresh another, a Symbol, an Integer in the
# fixnum range, nil and an object its own (#11078: a member the compiler
# could not show to be compared by pointer was refused).
require "set"

class Box
  attr_reader :seen
  def initialize = @seen = Set.new.compare_by_identity
end

b = Box.new
x = "ab" + "c"
o = Object.new
["abc", "abc", x, x.dup, :k, :k, 7, 7, nil, nil, o, o, [1], [1]].each { |m| b.seen << m }
p b.seen.size
p b.seen.include?("abc"), b.seen.include?(x), b.seen.include?("ab" + "c")
p b.seen.include?(:k), b.seen.include?(7), b.seen.include?(nil), b.seen.include?(o), b.seen.include?([1])
names = %w[a b a].map(&:to_sym).to_set.compare_by_identity
p names.size, names.include?(:a)
s = Set.new([-"q" + "r"]).compare_by_identity
p s.include?(-("q" + "r")), s.include?("qr"), s.compare_by_identity?
