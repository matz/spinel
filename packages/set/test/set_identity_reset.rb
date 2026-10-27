# reset on an identity Set answers the Set, and on a frozen one refuses, as
# CRuby's does: an identity Set has nothing to look up again, but it is
# still a mutation.
class K
  attr_reader :n
  def initialize(n) = @n = n
  def hash = @n.hash
  def eql?(o) = o.is_a?(K) && o.n == @n
end

a = K.new(1)
b = K.new(1)
s = Set.new.compare_by_identity
s << a
s << b
s << a
p [s.reset.equal?(s), s.size, s.include?(a), s.include?(K.new(1))]
# a plain Set that takes an identity Set's members compares as it does
pl = Set.new
pl << K.new(1)
pl.replace(s)
p [pl.compare_by_identity?, pl.size, pl.include?(b), pl.include?(K.new(1))]
s.freeze
begin
  s.reset
  p :no_error
rescue FrozenError => e
  p [e.class, e.message.start_with?("can't modify frozen Set")]
end
