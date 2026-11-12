# A map! or fill block whose first value arm is nil and a later arm is
# another kind (Symbol, Array, class) widens the Array so it holds both.
# The nil arm alone made the Array a nullable Integer one, and the later
# Symbol was stored as an Integer.

def tag(a) = a.map! { |x| x == 1 ? nil : :tag }
p tag([1, 2])

def wrap(a) = a.map! { |x| x == 1 ? nil : [x] }
p wrap([1, 2])

def klass(a) = a.map! { |x| x == 1 ? nil : Integer }
p klass([1, 2])

def three(a) = a.map! { |x| x == 1 ? nil : x == 2 ? 2.5 : :sym }
p three([1, 2, 3])

def nexts(a)
  a.map! do |x|
    next nil if x == 1
    :later
  end
end
p nexts([1, 2])

class Holder
  def initialize = @a = [1, 2, 3]
  def go = @a.map! { |x| x == 1 ? nil : :iv }
end
p Holder.new.go

Box = Struct.new(:a)
b = Box.new([1, 2])
b.a.map! { |x| x == 1 ? nil : :member }
p b.a

def filled(a) = a.fill { |i| i == 0 ? nil : :f }
p filled([0, 1, 2])

loc = [0, 1, 2]
loc.fill { |i| i == 0 ? nil : :g }
p loc

# a nil arm beside an Integer arm keeps the Integer Array
def ints(a) = a.map! { |x| x == 1 ? nil : x * 10 }
p ints([1, 2])
