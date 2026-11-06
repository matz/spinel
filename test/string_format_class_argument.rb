# String#% with a Class as its one argument formats the class's name, as
# CRuby does: a class named in the program, a builtin's, the class of a
# boxed value, and with a width.
class Edge; end

def describe(sym)
  "unknown symbol: %s" % sym.class
end

puts "a: %s" % String
puts "b: %s" % Edge
puts describe(1)
puts describe(:x)
puts describe(Edge.new)
puts "[%-8s]" % Integer
puts "%s!" % 3.class
