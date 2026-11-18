# The boxed argument of method_capture_boxed_arg.rb, where the Method is a
# local one write of which names its method and another does not: the
# second is a factory's result, so the call may run any captured method.
module First
  def self.check(value) = value
end
module Second
  def self.check(value) = value
end
def second_check = Second.method(:check)
def rewritten(flag, value)
  checker = First.method(:check)
  checker = second_check if flag
  checker.call(value)
end
p First.check(0), Second.check(0)
values = [1.5, "s", 2]
p rewritten(false, values[0]), rewritten(true, values[0]), rewritten(true, values[1])
p First.check(3), Second.check(4)
