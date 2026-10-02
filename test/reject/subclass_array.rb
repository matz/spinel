# A subclass of Array is built as a plain object: `push` and `last` raised
# NoMethodError at run time and `p` printed #<Stack> (#7075). Refused where
# it is declared until a subclass can be a real Array.
class Stack < Array
  def peek = last
end

s = Stack.new
s.push(1)
p s.peek
