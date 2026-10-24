# A push onto the Array a builtin answers for a boxed receiver stores what
# CRuby's Array stores: `x.to_s.scan(/\d+/) << 99`, where another class's
# to_s and scan make the call a dispatch, holds the Integer beside the
# Strings rather than raising. A program's own method of the name answers
# its own Array, which the push widens where it is built.

class Piece
  def scan(_re) = ["x"]
  def keys = [:k]
end

class Label
  def to_s = Piece.new
end

def version_of(name)
  (name.to_s.scan(/\d+/) << 99).join(".")
end

def tagged(s)
  s.scan(/\d+/).push(1.5, :t, nil).inspect
end

def same_kind(s)
  (s.scan(/\d+/) << "z").inspect
end

def dropped(s)
  s.scan(/\d+/) << 1
  s.scan(/\d+/).push(2)
  :done
end

def keyed(h)
  (h.keys << "z").inspect
end

p version_of(:V1_9_3)
p version_of("V4_0_7")
p version_of(Label.new)
p tagged("a1b22"), tagged(Piece.new)
p same_kind("a1b22"), same_kind(Piece.new)
p dropped("a1"), dropped(Piece.new)
p keyed({ a: 1 }), keyed({ 1 => 2 }), keyed(Piece.new)

# A method of the program that answers an Array it holds: the push goes
# into that Array itself, which the program reads back.
class Store
  def initialize
    @shared = ["a"]
  end

  def keys = @shared
  attr_reader :shared
end

def counted(h)
  (h.keys << 7).inspect
end

store = Store.new
p counted(store), store.shared
p counted({ b: 2 })

# Another class's class method aliased to the name: it runs on a class,
# and answers as the method it names does.
class Reader
  def self.lex(s) = [:lex, s]

  class << self
    alias scan lex
  end
end

p Reader.scan("s")
p version_of(Label.new), version_of("V2_1")

# A method answering a box that may be an Array an ivar holds: the push
# goes into that Array, not into a copy.
class Shelf
  def initialize(full)
    @full = full
    @items = ["a"]
  end

  def keys = @full ? @items : 1
  attr_reader :items
end

shelf = Shelf.new(true)
p counted(shelf), shelf.items

# Two classes answering literal Arrays of different kinds under one name.
class Words
  def parts = ["w"]
end

class Numbers
  def parts = [1]
end

def extended(x)
  (x.parts << :e).inspect
end

p extended(Words.new), extended(Numbers.new)

# A method a subclass overrides is still one the dispatch runs, for an
# instance of the class itself: its Array widens as the others do.
class Shape
  def parts = ["shape"]
end

class Circle < Shape
  def parts = ["circle"]
end

p extended(Shape.new), extended(Circle.new)
