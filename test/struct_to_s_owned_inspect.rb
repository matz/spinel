# Struct#to_s and Data#to_s are the builtin inspect under another name, not a
# call to self.inspect. A String member named inspect, or the program's own
# #inspect, changes what #inspect answers but leaves #to_s (and "#{s}") the
# default #<struct ...> text. The generated to_s called the class's inspect,
# so both took the owner's answer, and an #inspect built on to_s recursed
# until the stack ran out.
Shown = Struct.new(:inspect, :n)
Own = Struct.new(:a) do
  def inspect = "<#{to_s}>"
end
D = Data.define(:x) do
  def inspect = "dee"
end
Self = Struct.new(:me) do
  def inspect = "self"
end

s = Shown.new("shown", 2)
p s.inspect
puts s.to_s
puts "#{s}"
o = Own.new(1)
p o
puts o.to_s
puts Own.new(Own.new(5)).to_s
d = D.new(x: 3)
p d
puts "#{d}"

# the same through a boxed receiver
vals = [s, o, d, 7, "str"]
vals.each { |v| puts v.to_s; puts "#{v}" }
p vals

# a member holding the struct itself renders through the owner's inspect
me = Self.new(nil)
me.me = me
puts me.to_s
p me
