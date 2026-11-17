# A builtin's `new` as the value of a begin with a rescue or an ensure, of a method's rescue body and of a rescue
# modifier: the value is the object built, a Hash with a default among them, and reads after it see that object.
def hash_body
  Hash.new(3)
rescue TypeError
  nil
end
def array_body
  Array.new(2, 0)
rescue TypeError
  nil
end
def string_body
  String.new("x")
rescue TypeError
  nil
end
h = begin
  Hash.new(3)
rescue StandardError
  nil
end
p h, h && h[:k]
h = begin
  Hash.new(4)
ensure
  1
end
p h, h[:k]
h = (Hash.new(5) rescue nil)
p h, h && h[:k]
p hash_body, hash_body[:k]
a = begin
  Array.new(2, 0)
rescue StandardError
  nil
end
p a, a&.size
a = (Array.new(2, 1) rescue nil)
p a, array_body
s = begin
  String.new("x")
rescue StandardError
  nil
end
p s, s&.size
s = (String.new("y") rescue nil)
p s, string_body
