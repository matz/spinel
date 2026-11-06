# reduce / inject with a method-name Symbol on an object of a program class
# that defines its own: the Symbol is an argument, and no block is given,
# as CRuby passes them (#11117). The builtin fold still takes the Symbol.
class Gadget
  def reduce(initial = nil, op = nil)
    "initial=#{initial.inspect} op=#{op.inspect} block=#{block_given?}"
  end
end
p Gadget.new.reduce(0, :merge)
p Gadget.new.reduce(:merge)

class Pile
  def initialize(*xs) = @xs = xs
  def inject(sym) = "pile #{sym} #{@xs.size}"
end
p Pile.new(1, 2, 3).inject(:gcd)

p [12, 18, 24].reduce(:gcd), [4, 6].inject(1, :lcm), (1..4).reduce(:lcm)
