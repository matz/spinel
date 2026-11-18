# A nil handed to a constructor through `self.class.new` (a dup) or through
# `self.new` / `new` in a class method reaches initialize as nil, for the
# class itself and for each subclass the call can run for: a parameter
# typed from its default takes the nil, and a method called on it raises
# CRuby's NoMethodError, a keyword one as well as a positional one.
class Owner
  def v = 5
end

class Grid
  def initialize(size = 19, label = size.to_s)
    @size = size
    @label = label
  end

  def size = @size
  def label = @label
  def again = self.class.new(nil, @label)
  def blank = self.class.new(nil)
  def self.make = self.new(nil)
  def self.bare = new(nil, "bare")
end

class BigGrid < Grid
end

class Box
  def initialize(w = 1.5, owner = Owner.new, tags = ["t"], meta = { a: 1 })
    @w = w
    @owner = owner
    @tags = tags
    @meta = meta
  end

  def parts = [@w, @owner, @tags, @meta]
  def again = self.class.new(nil, nil, nil, nil)
end

class TallBox < Box
end

class Kw
  def initialize(size: 3)
    @size = size
  end

  def size = @size
  def again = self.class.new(size: nil)
end

class KwSub < Kw
end

class Account
  def initialize(owner: Owner.new)
    @value = owner.v
  end

  def value = @value
  def again = self.class.new(owner: nil)
end

class SubAccount < Account
end

class Named
  def initialize(n = 1)
    @n = n
  end

  def n = @n
  def again = self.class.new(nil)
end

class Shouty < Named
  def initialize(n = "b")
    @n = n.upcase
  end
end

class Counted < Named
  def initialize(n = 2)
    @n = n + 1
  end
end

g = Grid.new(5)
p g.again.size, g.again.label, g.blank.size, g.blank.label
p BigGrid.new(7).again.size, BigGrid.new(7).blank.label
p Grid.make.size, BigGrid.make.label, Grid.bare.label, BigGrid.bare.size
p Box.new.again.parts, TallBox.new.again.parts
p Kw.new.again.size, KwSub.new.again.size
p Named.new.again.n
[Account, SubAccount].each do |k|
  k.new.again
rescue NoMethodError => e
  puts "#{k}: NoMethodError #{e.message}"
end
[Shouty, Counted].each do |k|
  k.new.again
rescue NoMethodError => e
  puts "#{k}: NoMethodError #{e.message}"
end
