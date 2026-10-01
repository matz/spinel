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

# Both rest positions converge on one appender. A visited parameter is not
# a cached answer for the next rest position; asymmetric inputs detect a
# lost second bit and the POLY -> rest promotion must preserve both boxes.
def rest_grow(value)
  value << '!' if value.is_a?(String)
  nil
end
def rest_pair(left, right)
  rest_grow(left)
  rest_grow(right)
  nil
end
def rest_relay(*r)
  rest_pair(*r)
  nil
end
def rest_entry(value, other)
  rest_relay(other, value)
  nil
end
rest_entry(0, 0)
left = +'x'
right = +'seed'
alias_left = left
alias_right = right
rest_entry(left, right)
p left, right, alias_left, alias_right
begin
  rest_entry('frozen'.freeze, +'other')
rescue FrozenError
  p :rest_frozen
end

# A raw boxed direct store plus a readonly edge is not a transitive copy
# escape. Its existing container-handle promotion must still preserve aliases.
def direct_box_reader(value) = value.is_a?(String) ? value.bytesize : 0
def direct_box_store(value)
  items = []
  items.push(value)
  items[0] << '!' if value.is_a?(String)
  direct_box_reader(value)
  nil
end
direct_box_store(1)
raw = +'raw'
raw_alias = raw
direct_box_store(raw)
p raw, raw_alias
