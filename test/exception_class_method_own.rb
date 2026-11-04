# A class method `exception` of the program's answers before Exception's.
class Own < StandardError
  def self.exception(*a) = new("own #{a.size}")
end
class OwnBelow < Own; end
module Ext
  def exception(m) = new("ext #{m}")
end
class Extended < StandardError
  extend Ext
end
class Sing < StandardError
  class << self
    def exception(m) = new("sing #{m}")
  end
end
class Plain < StandardError; end
class NotAnError
  def self.exception(m = "d") = RuntimeError.new("not an error #{m}")
end

def show(e)
  puts "#{e.class} #{e.message.inspect}"
end

show Own.exception(1, 2)
show OwnBelow.exception
show Extended.exception("x")
show Sing.exception("q")
show NotAnError.exception
show NotAnError.exception("z")
show Plain.exception("still Exception's")
show RuntimeError.exception("and a builtin's")
[Own, OwnBelow].each { |k| show k.exception("v") }
