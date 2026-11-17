# defined?(obj.m) on an instance of a BasicObject subclass asks the lookup the
# call makes: a private def in a subclass hides the parent's attr_reader of the
# same name, so the call raises and defined? is nil.
class P < BasicObject
  attr_reader :x, :y
  def pub = 1
end
class C < P
  private def x = 1
end
o = C.new
p defined?(o.x)
p defined?(o.y)
p defined?(o.pub)
p defined?(o.nope)
p defined?(o.__id__)
