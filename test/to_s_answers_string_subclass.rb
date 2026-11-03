# A #to_s answering an instance of a String subclass: the instance is the
# String the caller reads, typed or boxed, interpolated or put

class SafeBuffer < String
  def html_safe? = true
end

class OutputBuffer
  def initialize(s) = @raw = s
  def to_s = SafeBuffer.new(@raw)
end

class Name
  def to_s = "name"
end

o = OutputBuffer.new("buf")
puts "<#{o}>"
puts o
print o, "\n"
s = o.to_s
p s.html_safe?
puts s + "!"

[1, OutputBuffer.new("boxed"), Name.new, :sym].each do |x|
  puts "[#{x}]"
  puts x
end
