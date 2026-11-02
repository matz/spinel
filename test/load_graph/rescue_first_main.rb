# A rescue LoadError clause runs whenever its begin runs only where a
# require of no file is the first statement of the begin's body: a
# statement before it may raise something else, which another clause
# catches, and the require never runs.
class RescueFirstA
  def m = "orig"
end
begin
  raise TypeError
  require "rescue_first_no_such_lib"
rescue LoadError
  class RescueFirstA
    def m = "fb"
  end
rescue TypeError
end
p RescueFirstA.new.m

class RescueFirstB
  def m = "orig"
end
def rescue_first_boom = raise(ArgumentError, "b")
begin
  rescue_first_boom
  require "rescue_first_no_such_lib"
rescue LoadError
  class RescueFirstB
    def m = "fb"
  end
rescue ArgumentError
end
p RescueFirstB.new.m
p RescueFirstB.instance_method(:m).owner
