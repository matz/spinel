# A slot held boxed dispatches an operator to a program class only when an
# object of that class can exist (#10902). P defines + and === but is never
# instantiated: the arm for it called sp_P__plus, whose body the C never
# emitted, and the `when` arm refused P#=== for a parameter nothing typed.
class P
  def +(x); x; end
  def ===(x) = true
end
class B
  def initialize; @v = 0; end
  def consume(len); @v += len; @v; end
end
b = B.new
puts b.send(:consume, ARGV.empty? ? 1 : "x")
puts b.send(:consume, ARGV.empty? ? 2 : "x")

# a subclass that inherits + is instantiated: its object still reaches P#+
class Q < P; end
b.instance_variable_set(:@v, Q.new)
puts b.consume(7)

def pick(pat)
  case 5
  when pat then "hit"
  else "miss"
  end
end
puts pick(ARGV.empty? ? 5 : "x")
puts pick(ARGV.empty? ? 6 : "x")
