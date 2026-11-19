# An instance variable nothing in the program sets reads nil, as in CRuby,
# rather than an untyped value. A `dup` handing one to `self.class.new`
# leaves initialize's parameter an Integer that may be nil, which a `||`
# default makes an Integer again (another class's write of the same name
# does not reach this class's objects), and a method answering one answers
# nil. A name a subclass, a module, an attr writer or a Struct member sets
# is not nil.
class Board
  def initialize(size = nil)
    @size = size || 19
    @cells = Array.new(@size * @size, 0)
  end

  def size = @size
  def count = @cells.size
end

class Game
  def initialize(size = 19, board = Board.new(size))
    @board = board
  end

  def board = @board
  def dup = self.class.new(@size, Board.new(@size))
  def size_of = @size
  def limit = (@limit || 7) * 3
end

class SmallGame < Game
  def initialize(size = 9, board = Board.new(size)) = super
end

g = Game.new(5).dup
p g.size_of, g.board.size, g.board.count, g.limit
p SmallGame.new.dup.board.size

class Mark
  def m = @m
end

class SetMark < Mark
  def initialize = (@m = 4)
end

module Tagging
  def tag! = (@tag = :t)
end

class Note
  include Tagging
  attr_writer :w
  def tag = @tag
  def w = @w
end

n = Note.new
p Mark.new.m, SetMark.new.m, n.tag, n.w
n.tag!
n.w = 2
p n.tag, n.w

# A Struct's or Data's members are set by its constructor, which no write
# spells: super into a member reader answers the member, in every mode.
class Span < Struct.new(:lo, :hi)
  def lo = super + 1
end

class Unit < Data.define(:n)
  def n = super * 2
end

p Span.new(1, 5).lo, Unit.new(n: 4).n

# The nil such a read answers is a value: a rescue modifier keeps it beside
# its rescue arm's Symbol, and an object parameter it reaches takes it.
class Owner2
  def v = 5
end

def owner_v(o) = o.nil? ? :none : o.v

class Lookup
  def initialize = (@b = 1)
  def sym = (@missing rescue :fallback)
  def num = (@missing rescue 3)
  def str = (@missing rescue "s")
  def owner = owner_v(@missing)
end

lk = Lookup.new
p lk.sym, lk.sym.nil?, lk.num.nil?, lk.str.nil?, (lk.sym || :d), lk.owner, owner_v(Owner2.new)

# A method answering such a read (or a literal nil) is a value in a rescue
# modifier too: a Range or boolean rescue arm, which holds no nil of its own,
# keeps the nil rather than reading it as 0..0 or false.
class Sized
  def initialize = (@b = 1)
  def sz = @size
  def none = nil
  def r = [(sz rescue (1..2)), (sz rescue true), (none rescue (3..4)), (none rescue false)]
end

p Sized.new.r, Sized.new.r.map(&:nil?)

# A Range end held as nil is the open bound a written nil is, a String
# range's too, and a case/in subject held as nil matches `in nil`: an ivar
# nothing sets, a method answering nil and a written nil alike.
class Spans
  def initialize = (@b = 1)
  def none = nil
  def r = [(@missing..5), (1..@missing), (@missing.."z"), ("a"..@missing), (none..3), (nil.."e"), ("b"..nil)]
  def m = [(case @missing; in nil then :n; in Integer then :i; end), (case nil; in nil then :n; end)]
end

sp = Spans.new
p sp.r, sp.r.map { |x| [x.begin, x.end] }, sp.m

# An inject seed that is nil -- the read, a method answering it, a method
# answering a written nil -- starts a boxed accumulator, as `inject(nil)`
# does, whatever the elements are.
class Folds
  def initialize = (@b = 1)
  def seed = @missing
  def none = nil
  def m = [[1, 2].inject(@missing) { |s, x| (s || 0) + x }, [1, 2].inject(seed) { |s, x| (s || 0) + x },
           [1.5].inject(none) { |s, x| (s || 0) + x }, %w[a b].inject(seed) { |s, x| (s || "") + x },
           [3, 4].each_with_index.inject(seed) { |s, (x, i)| (s || 0) + x * i }]
end

p Folds.new.m

# defined? asks whether the ivar is set, through a call on it as well: a
# read under a defined? keeps its own answer, set to nil or never set.
class Asks
  def chk = defined?(@missing.nil?)
  def chk2 = defined?(@missing.to_s.size)
  def chk3 = defined?(@missing)
  def val = @missing.nil?
end
class AsksSet
  def chk = defined?(@x.nil?)
  def set = (@x = nil)
end
q = Asks.new
p [q.chk, q.chk2, q.chk3, q.val]
qs = AsksSet.new
p qs.chk
qs.set
p qs.chk

# A splat of a method answering the read adds nothing to an array literal,
# and the method still runs
class Spreads
  def f
    puts "f ran"
    @missing
  end
  def m = [[0, *f], [0, "s", *f], [1.5, *f]]
end
p Spreads.new.m
