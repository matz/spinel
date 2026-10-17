# An inherited class method calling a sibling class method that answers
# nil in the base class and an Integer in a subclass: the site's Integer
# can be nil, for the base class's own run of the body. It was typed the
# subclass's Integer, and the base class's run boxed the nil call's void
# as one (a C "passing 'void' to parameter of type 'sp_int'" error).
class Base
  def self.dp = nil
  def self.make(v) = v.nil? ? dp : v

  def self.port_or_default(port)
    port = dp if port.nil?
    port
  end
end

class Sub < Base
  def self.dp = 80
end

class Deeper < Sub
  def self.dp = nil
end

p Sub.make(nil)
p Base.make(nil)
p Sub.make(5)
p Deeper.make(nil)
p Base.port_or_default(nil)
p Sub.port_or_default(nil)
p Sub.port_or_default(8080)
