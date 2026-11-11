# An Array subclass that only `K.new(..)` builds, K a constant holding a
# Class value, is in the census of classes that can exist from the start:
# an op-write on a box holding one runs its own `[]` and `[]=`.
class MyA < Array
  def [](i)
    puts "MyA#[] #{i}"
    super
  end

  def []=(i, v)
    puts "MyA#[]= #{i} #{v}"
    super
  end
end

class Foo
  def tag = 1
end

def pick(i) = i == 0 ? MyA : Array
K = pick(ARGV.size)
b = [Foo.new, K.new([1, 2])][ARGV.size + 1]
b[0] += 10
p b, b.class
