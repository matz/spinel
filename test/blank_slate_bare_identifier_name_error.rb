# A bare identifier in a method of a BasicObject subclass that names a
# method of a module included at the top level (Object's, which a blank
# slate is not under) is CRuby's NameError with its wording; the same name
# called with parentheses is NoMethodError.
module T
  def hello = "hi"
end
include T
class Blank < BasicObject
  def greet
    hello
  rescue ::NameError => e
    e.message
  end
  def greet2
    hello()
  rescue ::NameError => e
    e.message
  end
end
$stdout.puts Blank.new.greet
$stdout.puts Blank.new.greet2
class Blank2 < BasicObject
  def a
    nope
  rescue ::NameError => e
    [e.class, e.message, e.name]
  end
  def b
    nope()
  rescue ::NameError => e
    [e.class, e.message, e.name]
  end
end
$stdout.puts Blank2.new.a.inspect, Blank2.new.b.inspect
