# A public top-level def beside a class that answers the same name by an
# attribute or a Struct member: an instance of the class in a box takes its
# own, not the top level's def.
class Foo
  attr_reader :pa
  def initialize = @pa = :attr
end
def pa = :top
public :pa
x = [1, Foo.new][ARGV.size + 1]
p x.pa
[Foo.new, Foo.new].each { |f| p f.pa }

Pair = Struct.new(:left, :right)
def left = :top_left
public :left
y = ["s", Pair.new(1, 2)][ARGV.size + 1]
p y.left
