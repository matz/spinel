# The same, where a prepended initialize that forwards with `super` fills the builtin's argument from its default.
module HashDefault
  def initialize(d = 3) = super
end
module ArrayDefault
  def initialize(n = 2, v = 0) = super
end
Hash.prepend(HashDefault)
Array.prepend(ArrayDefault)
def hash_body
  Hash.new
rescue TypeError => e
  p e.class
end
def array_body
  Array.new
rescue TypeError
  nil
end
h = begin
  Hash.new
rescue StandardError
  nil
end
p h, h && h[:k]
h = begin
  Hash.new
ensure
  1
end
p h, h[:k]
h = (Hash.new rescue nil)
p h, h && h[:k]
p hash_body, hash_body[:k]
a = begin
  Array.new
rescue StandardError
  nil
end
p a, a&.size
a = (Array.new rescue nil)
p a, array_body
