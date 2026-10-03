# Nokogiri's coerce -> fragment -> initialize -> fill -> parse only reads
# the markup. A boxed parameter forwarded beyond the old depth bound must
# not be diagnosed as an append. Keep the caller's alias and binary bytes.
class MarkupReader
  def coerce(data)
    return fragment(data) if data.is_a?(String)
    0
  end
  def fragment(tags) = one(tags)
  def one(tags) = two(tags)
  def two(tags) = three(tags)
  def three(tags) = tags.is_a?(String) ? tags.bytesize : 0
end

reader = MarkupReader.new
p reader.coerce(nil)
text = +"a\0bc"
other = text
p reader.coerce(text), text.bytes, other.bytes

# A read-only cycle must terminate in the analysis too.
def read_a(value, n) = n > 0 ? read_b(value, n - 1) : value.to_s.bytesize
def read_b(value, n) = read_a(value, n)
read_a(nil, 0)
p read_b(text, 5), text.bytes

# Different parameters of the same method are different graph vertices.
# The back edge appears before the appender: stopping at that edge must
# not hide the real mutation, even through an alias and an exception.
LONG = "!" * 100
def swap_a(left, right, n)
  swap_a(right, left, n - 1) if n > 0
  left << LONG if left.is_a?(String)
  raise ArgumentError, "after append" if n == 1
  nil
end
swap_a(nil, nil, 0)
a = +"x"
b = +"yz"
alias_a = a
alias_b = b
begin
  swap_a(a, b, 1)
rescue ArgumentError => e
  p e.message
end
p a.bytesize, b.bytesize, alias_a.bytesize, alias_b.bytesize
begin
  swap_a("f".freeze, nil, 0)
rescue FrozenError
  p :frozen
end

# Rest forwarders and POLY entry points preserve the caller's alias when
# recursion reaches an appender, including entry from both directions.
def mixed_a(value, n) = mixed_b(n, value)
def mixed_b(n, *args)
  mixed_a(*args, n - 1) if n > 0
  mixed_grow(*args) if n == 0
  nil
end
def mixed_grow(value) = (value << LONG if value.is_a?(String); nil)
mixed_a(nil, 0)
c = +"c"
alias_c = c
mixed_a(c, 2)
mixed_b(2, c)
p c.bytesize, alias_c.bytesize

# Explicit keyword values are forwarding edges, not retained Hash elements.
# Both the positional POLY entry and keyword destination must be examined.
def keyword_entry(value) = keyword_middle(value)
def keyword_middle(value) = keyword_leaf(text: value)
def keyword_leaf(text:) = text.is_a?(String) ? text.bytesize : 0
keyword_entry(1)
p keyword_entry(text), text.bytes, other.bytes
