# `x.itself` on an Integer that may be nil answers x, nil included, while
# another class of the program defines its own `itself`: the call is still
# Kernel's for the Integer, in the Integer's nullable form. A splat call's
# desugar wraps its arms in `.itself` too.
class U
  def itself = :mine
end
p U.new.itself
w = [1, 2].index(ARGV.size == 0 ? 9 : 2).itself
p w
p [1, 2].index { |x| x == 2 }.itself + 1
def f(*a) = [5, 6].find_index(*a) { |x| x > 5 }
p f(9), f(6), f
y = [1].tap(&:shift).first
p y.itself, y.itself.nil?
