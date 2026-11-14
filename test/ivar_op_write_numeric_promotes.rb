# spinel: gc-stress
# A numeric ivar's arithmetic op-write (+= -= *= /= %= **=) answers the
# promotion of the slot and the operand, as a local's does: `@wins = 0.0`
# bumped by `@wins += 1` stays a Float. The write took the operand's type,
# Integer, and an Integer write beside the Float one boxed the slot and
# every read of it. An Integer slot bumped by a Float holds both and stays
# boxed, as does one bumped by a boxed operand; a shift or bitwise op-write
# keeps the operand's type.

class Acc
  def initialize
    @f = 0.0      # Float bumped by Integers
    @g = 10.0     # each arithmetic op-write
    @i = 0        # Integer bumped by Floats: holds both, stays boxed
    @n = 0        # Integer bumped by Integers
    @r = 1r       # Rational
    @c = 1.5      # Float bumped by a boxed operand
  end

  def run(k, mixed)
    @f += 1
    @f += k
    @g -= 1
    @g *= 2
    @g /= 4
    @g **= 2
    @g %= 7
    @i += 0.5
    @n += k
    @r += 1
    @c += mixed
    self
  end

  def show = p([@f, @g, @i, @n, @r, @c, @f.class, @i.class])
end

a = Acc.new
mixed = [1, 2.5][ARGV.size]
3.times { |k| a.run(k, mixed) }
a.show

# a Float ivar's ratio, as a tree search's win rate is computed
class Node
  attr_reader :wins, :visits
  def initialize = (@wins = 0.0; @visits = 0)
  def won = (@visits += 1; @wins += 1)
  def lost = (@visits += 1)
  def rate = @wins / @visits
end
n = Node.new
n.won; n.lost; n.won; n.won
p n.rate, n.wins, n.visits

# a class-level ivar, a toplevel one, `||=` before `+=`, and a division by zero
class Counter
  @total = 0.0
  def self.bump = (@total += 1)
  def self.total = @total
end
Counter.bump; Counter.bump
p Counter.total
@t = 0.5
def tick = (@t += 2)
tick; tick
p @t
class Lazy
  def go = ((@v ||= 0.25); @v += 3; @v)
end
p Lazy.new.go
class Div
  def initialize = (@d = 1.0)
  def go = (@d /= 0; @d)
end
p Div.new.go

# a shift on a Float slot raises NoMethodError, as in CRuby
class Shift
  def initialize = (@s = 1.0)
  def go = (@s <<= 1)
end
begin
  Shift.new.go
rescue NoMethodError => e
  p e.class
end
