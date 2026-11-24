# A program's own Object#inspect and Object#to_s answer for an object whose
# class defines neither, in p, puts, print, interpolation and a container's
# render as in an explicit call: CRuby's lookup reaches a reopened Object
# ahead of Kernel. A class's own method still answers first.
class Object
  def inspect = "LOUD"
  def to_s = "loud"
end
class K; end
class Own
  def to_s = "own"
end
k = K.new
p k
puts k
print k, "\n"
puts "#{k}"
p [k]
puts [k]
p k.inspect
puts Own.new
p Own.new
