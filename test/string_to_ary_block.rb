# A program's String#to_ary is called when a lone String meets a block that
# takes it apart, as CRuby's rb_check_array_type does: a yield, an each, map
# or each_with_index over Strings, a proc call, a boxed String and a multiple
# assignment. A to_ary that answers nil keeps the String, a non-Array answer
# is a TypeError, and a block that takes the String whole (`|a|`, `|*r|`)
# never asks.
class String
  def to_ary
    puts :to_ary
    [self, +"tail"]
  end
end

def yl(v) = yield(v)
s = +"s"
bx = [s, 1].first

yl(s) { |a, b| p [a, b] }
yl(bx) { |a, b| p [a, b] }
yl(s) { |a, | p [a] }
yl(s) { |a, *r| p [a, r] }
yl(s) { |a, b = 7| p [a, b] }
yl(s) { |(a, b), c| p [a, b, c] }
yl(s) { |a| p a }
yl(s) { |*r| p r }
yl(bx) { |a| p a }

[s].each { |a, b| p [a, b] }
p [s].map { |a, b| b }
[s].each_with_index { |(a, b), i| p [a, b, i] }
"ab".each_char { |a, b| p [a, b] }

pr = proc { |a, b| [a, b] }
p pr.call(s)
p pr.call(bx)
p pr.(s)

a, b = s
p [a, b]
a, b = bx
p [a, b]
(a, b), c = s, 1
p [a, b, c]
a, b = *[s]
p [a, b]

def lead(v) = yield(v, 1)
lead(s) { |a, b| p [a, b] }

class MyS < String; end
m = MyS.new("m")
yl(m) { |a, b| p [a, b] }
a, b = m
p [a, b]

# an answer that is not an Array: nil keeps the String, anything else raises
class String
  def to_ary = $answer
end
$answer = nil
yl(s) { |a, b| p [a, b] }
yl(bx) { |a, b| p [a, b] }
a, b = s
p [a, b]
$answer = 42
begin
  yl(s) { |a, b| p [a, b] }
rescue TypeError => e
  puts e.message
end
begin
  yl(bx) { |a, b| p [a, b] }
rescue TypeError => e
  puts e.message
end
$answer = [1, 2]
yl(s) { |a, b| p [a, b] }

# the forms with a rest, a post or an optional over typed Strings
class String
  def to_ary = [self, :tail]
end
o = +"o"
[o].each { |a, *r| p [a, r] }
[o].each { |a, b = 7| p [a, b] }
[o].each { |*r, z| p [r, z] }
[o].each { |a, b = 7, c| p [a, b, c] }
[o].map { |a, | a }.then { p _1 }
[o].each { p [_1, _2] }
[o].each { |a, *| p a }
yl(o) { |a, *r| p [a, r] }
yl(o) { |*r, z| p [r, z] }

# a String subclass with a to_ary of its own, typed and boxed
class Sub < String
  def to_ary = [self, :sub]
end
sub = Sub.new("m")
x, y = sub
p [x, y]
yl(sub) { |a, b| p [a, b] }
yl([sub, 1].first) { |a, b| p [a, b] }
yl(o) { |a, b| p [a, b] }
