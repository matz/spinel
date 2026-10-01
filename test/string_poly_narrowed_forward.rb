# Integer seeds are essential: String and nil alone can settle on a
# nullable String slot and miss re-boxing a narrowed POLY local.
def grow(value, ignored = 0)
  value << "!" if value.is_a?(String)
  nil
end
def guarded(value)
  grow(value) if value.is_a?(String)
  nil
end
def conditional(value)
  value.is_a?(String) ? grow(value) : nil
end
def ordered(value)
  if value.is_a?(String)
    old = value
    grow(value, (value = +"replacement"; 0))
    p old, value
  end
  nil
end
grow(1)
guarded(1)
conditional(1)
ordered(1)
text = +"abc"
other = text
guarded(text)
conditional(text)
p text, other
ordered(+"old")
begin
  guarded("frozen".freeze)
rescue FrozenError
  p :frozen
end

class PolyAppender
  def coerce(data)
    return fragment(data) if data.is_a?(String)
    nil
  end
  def fragment(tags) = one(tags)
  def one(tags) = two(tags)
  def two(tags) = three(tags)
  def three(tags) = four(tags)
  def four(tags) = five(tags)
  def five(tags) = grow(tags)
  def grow(tags)
    tags << "?" if tags.is_a?(String)
    nil
  end
end
reader = PolyAppender.new
reader.coerce(1)
reader.fragment(1)
text = +"a\0bc"
other = text
reader.coerce(text)
p text.bytes, other.bytes
