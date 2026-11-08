# A computed send on an object of a program's class reaches what the program
# defines for every object: a method of an Object or Kernel reopening, and,
# for send alone, a top-level def. A blank slate (`< BasicObject`) reaches
# none of them.
class Object
  def try(*args, **kw) = [args, kw]
  def tag = "object"
end
module Kernel
  def kern = :kernel
end
def helper(x = 1) = x * 2

class Box; end
class Crate < Box
  def own = :own
end

m = [:try, :x][0]
p Box.new.send(m), Crate.new.send(m, 1, k: 2)
n = "ta" + "g"
p Box.new.send(n), Crate.new.public_send(n)
p Crate.new.send("#{[:own, :x][0]}"), Box.new.send("k" + "ern")
h = "help" + "er"
p Box.new.send(h), Crate.new.send(h, 5)
p((Box.new.public_send(h) rescue $!.class))

class Blank < BasicObject
  def own = :blank
end
bl = Blank.new
["own", "tag"].each do |s|
  s += ""
  r = (bl.__send__(s) rescue $!.class)
  p [s, r]
end
