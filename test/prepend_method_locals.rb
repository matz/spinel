# A prepended module's method keeps its locals: `r = super` and the rest
module Twice
  def value
    r = super
    x = 2
    r * x
  end

  def names
    out = []
    super.each { |n| up = n.upcase; out << up }
    out
  end
end

class Box
  prepend Twice
  def value = 21
  def names = ["a", "b"]
end

class Crate
  prepend Twice
  def value = 5
  def names = ["c"]
end

p Box.new.value
p Box.new.names
p Crate.new.value
p Crate.new.names
