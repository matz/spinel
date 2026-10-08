# Kernel#Pathname(): a String becomes a Pathname, a Pathname is answered as
# the same object, and the result is a full Pathname. Every expectation is
# CRuby's own output.
require "pathname"
a = Pathname("/tmp/x/y.txt")
b = Pathname(a)
puts a.class
puts a.to_s
puts b.equal?(a)
puts Pathname("rel/y").dirname.to_s
puts Pathname("a.rb").extname
puts (Pathname("/a") + "b").to_s
puts Pathname("/a/b").relative_path_from(Pathname("/a")).to_s
puts Pathname("x") == Pathname.new("x")
