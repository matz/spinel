# spinel: gc-stress
# Captures keep shallow String and object identity after the only source is cleared.
class Item
  def initialize(n); @n = n; end
  def n; @n; end
  def n=(n); @n = n; end
end
$strings = [+"a"]
$objects = [Item.new(7)]
class ClearStrings
  def to_ary
    $strings[0] << "!"
    $strings.clear
    GC.start
    []
  end
end
class ClearObjects
  def to_ary
    $objects[0].n = 9
    $objects.clear
    GC.start
    []
  end
end
string_id = $strings[0].object_id
x = [$strings, 0].first
y = [ClearStrings.new, 0].first
a = [0, "r"]
a.concat(x, y)
p a.last
p a.last.object_id == string_id
object_id = $objects[0].object_id
x = [$objects, 0].first
y = [ClearObjects.new, 0].first
a = [0, "r"]
v = a.concat(x, y)
p v.equal?(a)
p a.last.n
p a.last.object_id == object_id
