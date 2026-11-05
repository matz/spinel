# Range#include? / #member? on program objects: CRuby walks by succ and
# compares with ==, refuses an open end, and answers by cover? only for a
# Numeric or an end answering to_int.
class V
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def succ = V.new(n + 1)
end

class N < Numeric
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def succ = N.new(n + 1)
end

class T
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def succ = T.new(n + 1)
  def to_int = n
end

r = V.new(1)..V.new(5)
p r.include?(V.new(3))
p r.include?(V.new(2.5))
p r.member?(V.new(2.5))
p r === V.new(2.5)
p r.cover?(V.new(2.5))
p (V.new(1)...V.new(5)).include?(V.new(5))
p (V.new(1)..V.new(5)).include?(V.new(5))
[(V.new(1)..), (..V.new(5))].each do |o|
  begin
    p o.include?(V.new(3))
  rescue TypeError => e
    p [e.class, e.message]
  end
  p o === V.new(3)
end
p (N.new(1)..N.new(5)).include?(N.new(2.5))
p (N.new(1)..).include?(N.new(2.5))
p (T.new(1)..T.new(5)).include?(T.new(2.5))

boxed = [V.new(1)..V.new(5), 1..5, "a".."e"]
boxed.each { |b| p b.include?(b.first.succ) }
p boxed[0].include?(V.new(2.5))
p boxed[0].member?(V.new(2.5))

# to_int is called: one answering nil leaves the walk, one answering an
# Integer takes cover?
class U
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def succ = U.new(n + 2)
  def to_int = nil
end
class K
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def succ = K.new(n + 2)
  def to_int = n
end
p (U.new(1)..U.new(5)).include?(U.new(2))
p (U.new(1)..U.new(5)).include?(U.new(3))
p (K.new(1)..K.new(5)).include?(K.new(2))
# an open end is refused whatever the argument, a Range too
begin
  p (U.new(1)..).include?(U.new(1)..U.new(2))
rescue TypeError => e
  p e.message
end

# a Range built from boxed ends becomes a Range of objects at run time, and
# its ends are classified the same way
bv = [N.new(1), 1, K.new(1)]
br = (bv[0]..)
p br.include?(N.new(2.5))
bk = (bv[2]..bv[2].succ.succ)
p bk.include?(K.new(2))
