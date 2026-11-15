# A program class's == that only a builtin calls, on an Array or a Hash
# that reaches the call boxed, is kept: include?, index, count and delete
# compare the elements with it, and a Hash compares its values with it.
class Ice
  attr_reader :n
  def initialize(n) = @n = n
  def ==(o)
    puts "eq #{n}"
    o == :ice
  end
end

arr = [Ice.new(1), :x]
b = [arr, 1]
p b[0].include?(:ice), b[0].index(:ice), b[0].count(:ice)
b[0].delete(:ice)
p arr.size

h = { a: Ice.new(2) }
c = [h, 1]
p c[0].value?(:ice), c[0].key(:ice)
