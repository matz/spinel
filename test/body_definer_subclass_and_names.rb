# A definer in a define_singleton_method's block of a class that has a subclass runs on the class its method is
# called on, so a call through the subclass defines there and the class itself still has no reader; where the
# class has no subclass the block's reader is a version. A reader whose name is a definer's (an attr_reader named
# attr_reader) is called as the object's own method in a define_method block, not Module's definer. CRuby prints
# NoMethodError three times, 4, then [7, 8].
class Base
  def initialize = (@late = 3)
  define_singleton_method(:install) { attr_reader :late; nil }
end
class Child < Base; end
p((Base.new.late rescue $!.class))
Child.install
p((Base.new.late rescue $!.class))

class Solo
  def initialize = (@ready = 4)
  define_singleton_method(:install) { attr_reader :ready; nil }
end
p((Solo.new.ready rescue $!.class))
Solo.install
p Solo.new.ready

class Named
  attr_reader :attr_reader, :define_method
  def initialize = (@attr_reader = 7; @define_method = 8)
  define_method(:probe) { [attr_reader(), define_method()] }
end
p Named.new.probe
