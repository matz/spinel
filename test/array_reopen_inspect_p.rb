# p runs a program's own Array#inspect. It printed every Array with the
# builtin inspect: `p [1, 2]` gave [1, 2] for the method's "<2 items>", and
# so did an Array held in a Hash, a boxed one, and one an object's default
# inspect lists, while an explicit `.inspect` call ran the method.

class Array
  def inspect = "<#{size} items>"
end

p [1, 2]
p [1.5]
p ["a", "b", "c"]
p [:s]
p [1, "a"]
p [[1], [2, 3]]
p [], [7], [8, 9]
p Array.new, Array.new(2, 0)

# a table pushed into, and a local
x = [[1, 2], [3]]
x[0] << 9
p x

# an Array held in a Hash, a Struct, an object, or boxed
p({k: [1, 2], j: "s"})
h = {a: [7.5]}
p h
s = Struct.new(:xs).new([3])
p s
class Holder
  def initialize = (@items = [4, 5, 6])
end
p Holder.new.instance_variable_get(:@items)
p [[1], "s"][0]

# an explicit inspect, and to_s, which the program does not reopen
puts [3, 4].inspect
puts [3, 4].to_s
puts "#{[5]}"
print [6], "\n"
