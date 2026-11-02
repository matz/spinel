# A call on a boxed value keeps boxed only the tables of the classes it holds
# A call on a value of no static type may reach any same-named method, and a
# table that method hands out or takes in stays boxed for it. The class
# facts bound such a value's classes: `x` below is a Scan or a Label, so
# `x.items` reaches neither Reader's reader nor Ledger's (Ledger's subclass
# Journal is no value of x either), and `s.pick` reaches no Ranker, whose
# parameter takes Board's table. Those tables narrow; Scan's, which
# `x.items` does reach, and Shelf's, which a call on a Book reaches through
# the reader Book inherits, stay boxed. So does the parameter of Press.pick,
# a class method a call on a value that may be the Press class reaches.

Item = Struct.new(:id, :w)

class Scan
  attr_reader :items
  def initialize(n) = @items = Array.new(n) { |i| Item.new(i, i) }
end

class Label
  attr_reader :items
  def initialize = @items = %w[a b]
end

class Reader
  attr_reader :items
  def initialize(n) = @items = Array.new(n) { |i| Item.new(i, i * 2) }
end

class Ledger
  attr_reader :items
  def initialize(n) = @items = Array.new(n) { |i| Item.new(i, i * 3) }
end
class Journal < Ledger; end

class Shelf
  attr_reader :items
  def initialize(n) = @items = Array.new(n) { |i| Item.new(i, i * 4) }
end
class Book < Shelf; end

class Sorter
  def pick(t) = t.max_by(&:w)
end

class Ranker
  def pick(t) = t.first
end

class Board
  def initialize = @rows = Array.new(3) { |i| Item.new(i, 10 - i) }
  def best(by) = by.pick(@rows)
end

class Press
  def self.pick(rows) = rows.first
end

class Desk
  def initialize = @rows = Array.new(2) { |i| Item.new(i, 20 + i) }
  def top = Press.pick(@rows)
end

p Reader.new(4).items.map(&:w).max, Journal.new(2).items.map(&:w)
x = [Scan.new(2), Label.new][ARGV.size]
p x.items.size
y = [Book.new(3), Label.new][ARGV.size]
p y.items.map(&:w)
s = [Sorter.new, "s"][ARGV.size]
p s.pick([Item.new(1, 5), Item.new(2, 7)]).w
p Board.new.best(Ranker.new).w
p Desk.new.top.w
k = [Press, "k"][ARGV.size]
p k.pick([1, "two", 3.0])
