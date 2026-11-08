# A typed Array's push, append, unshift, prepend or concat given only a
# block-pass operand converts it: an object's to_proc runs once per call,
# though the mutator never calls the block. (A program defining to_proc keeps
# the compiler from checking a bad operand's type, so that is another test.)
class Adder
  def to_proc
    $n += 1
    proc { |*a| a }
  end
end
$n = 0
p [].push(&Adder.new)
p $n
a = [1]
p a.append(&Adder.new)
p $n
p a.unshift(&Adder.new)
p $n
p a.prepend(&Adder.new)
p $n
p a.concat(&Adder.new)
p $n
