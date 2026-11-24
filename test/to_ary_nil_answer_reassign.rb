# A to_ary that answers nil keeps the very object it was asked of, also when
# its body reassigns the variable the multiple assignment read it from: the
# value is evaluated once, before to_ary runs.
$hook = nil
class String
  def to_ary
    $hook&.call
    return nil if start_with?("n")
    [self, 1]
  end
end
class Pair
  def initialize(skip) = @skip = skip
  def to_ary
    $hook&.call
    return nil if @skip
    [self, 2]
  end
end

$g = +"nil"
$hook = -> { $g = +"other" }
a, b = $g
p [a, b]

class Holder
  @@c = +"nil"
  def initialize = @s = +"nil"
  def run
    $hook = -> { @s = +"changed" }
    a, b = @s
    p [a, b]
    $hook = -> { @@c = +"other" }
    a, b = @@c
    p [a, b]
  end
  def param(s)
    $hook = -> { s = +"other" }
    a, b = s
    p [a, b]
  end
end
Holder.new.run
Holder.new.param(+"nil")

$g = +"nil"
$hook = -> { $g = +"other" }
x = (a, b = $g)
p [x, a, b]

s = +"nil"
$hook = -> { s = +"other" }
a, b = s
p [a, b, s]

$g = Pair.new(true)
$hook = -> { $g = Pair.new(false) }
a, b = $g
p [a.class, b, a.equal?($g)]
@iv = Pair.new(true)
$hook = -> { @iv = nil }
a, b = @iv
p [a.class, b]
$hook = nil
