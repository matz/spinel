# The FrozenError a frozen exception subclass instance raises at an ivar
# write in a method it inherits has that instance as its receiver, of its
# own class: boxed as the method's class, it answered WidgetError and was
# not a GadgetError (#11617).
class WidgetError < StandardError
  def locate(line)
    @line = line
    self
  end
end

class GadgetError < WidgetError
end

e = GadgetError.new("bad gear")
puts e.locate(1).class
e.freeze
begin
  e.locate(4)
rescue FrozenError => ex
  r = ex.receiver
  puts r.class
  puts r.equal?(e)
  puts r.is_a?(GadgetError)
end

class Plain
  def set(v)
    @v = v
  end
end
class Sub < Plain
end
s = Sub.new.freeze
begin
  s.set(1)
rescue FrozenError => ex
  puts ex.receiver.class
end

e2 = GadgetError.new("worn")
e2.freeze
begin
  e2.locate(5)
rescue FrozenError => ex
  r = ex.receiver
  puts r.message
  puts r.is_a?(WidgetError), r.is_a?(StandardError)
  begin
    raise r
  rescue GadgetError => g
    puts "rescued #{g.class}: #{g.message}"
  end
end
