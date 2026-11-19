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
