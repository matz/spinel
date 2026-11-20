# Splatting a beginless or endless Integer Range raises as its to_a does
# (TypeError / RangeError, the endless check first): the walk from the
# open bound's sentinel allocated until memory ran out, or answered [].
def lo = nil
def hi = nil
x = nil
class R
  def a = [*(@lo..3)]
  def b = [*(1..@hi)]
end
def f(*a) = a.size
[-> { [*(lo..3)] }, -> { [*(1..hi)] }, -> { [*(x..3)] }, -> { [*(1..x)] }, -> { [*(x..x)] },
 -> { R.new.a }, -> { R.new.b }, -> { [0, *(lo...3)] }, -> { f(*(x..3)) }, -> { [1.0, *(x..3)] },
 -> { [*(1..3)] }, -> { [*(1...3), *(5..6)] }, -> { [*(3..1)] }].each do |g|
  begin
    p g.call
  rescue => e
    p [e.class, e.message]
  end
end

# `when *range` spreads the Range the same way: its members match, and an
# open one raises
y = nil
[-> { case 2 when *(lo..3) then :in else :out end }, -> { case 2 when *(y..3) then :in else :out end },
 -> { case 2 when *(1..y) then :in else :out end }, -> { case 2 when *(1..3) then :in else :out end },
 -> { case 5 when *(1...5) then :in else :out end }, -> { r = (1..3); case 3 when *r then :in else :out end },
 -> { case "b" when *("a".."c") then :in else :out end }].each do |g|
  begin
    p g.call
  rescue => e
    p [e.class, e.message]
  end
end

# ...in a case statement too, and with a subject of a value class, whose
# box is a fresh copy the Range of them must not collect
class Rung
  include Comparable
  attr_reader :n
  def initialize(n) = (@n = n)
  def <=>(o) = n <=> o.n
  def succ = Rung.new(n + 1)
end
r = (1..3)
[-> { o = :miss; case 2 when *r then o = :hit end; o }, -> { o = :miss; case 2 when *(1..3) then o = :hit end; o },
 -> { o = :miss; case "b" when *("a".."c") then o = :hit end; o }, -> { o = :miss; case 2 when *(1..y) then o = :hit end; o },
 -> { o = :miss; case 2 when *(lo..3) then o = :hit end; o }, -> { s = 0; [1, 2, 5].each { |v| case v when *(1..3) then s += 1 end }; s },
 -> { case Rung.new(2) when *(Rung.new(1)..Rung.new(3)) then :in else :out end }].each do |g|
  begin
    p g.call
  rescue => e
    p [e.class, e.message]
  end
end

# A value that is not an Array spreads as a splat does: a String or Symbol
# as [x] (its bytes read as nils matched a nil subject), an Enumerator as
# its items, a Float Range raises; and a fresh subject stays alive while
# the Range is spread
SPL = (1..3)
W = "ab"
def nothing(x) = x
def fresh = [1, 2]
[-> { o = :miss; case nothing(nil) when *W then o = :hit end; o }, -> { case nothing(nil) when *:ab then :hit else :miss end },
 -> { o = :miss; case "ab" when *W then o = :hit end; o }, -> { case 2 when *[1, 2].each then :hit else :miss end },
 -> { o = :miss; case 1.5 when *(1.0..2.0) then o = :hit end; o }, -> { case fresh when *SPL then 1 else 2 end },
 -> { o = 2; case fresh when *SPL then o = 1 end; o }].each do |g|
  begin
    p g.call
  rescue => e
    p [e.class, e.message]
  end
end

# each element is asked once, as `elem === subject`: a pattern whose == has
# state sees one call
class Counted
  def initialize = (@calls = 0)
  def ==(o) = (@calls += 1; @calls > 1)
  attr_reader :calls
end
cp = Counted.new
p [(case Object.new when *[cp] then :in else :out end), cp.calls]
