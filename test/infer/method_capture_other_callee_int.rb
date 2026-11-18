# A boxed argument at a call that cannot run a captured method leaves that
# method's sp_int lane alone (method_capture_boxed_arg.rb is the program
# whose boxed argument widens one): `call` on a boxed receiver that is one
# of two classes of the program's own, `[]` on a boxed Array, Hash or
# String, and a Method local whose every write names another method.
class Doubler
  def call(value) = value.is_a?(Integer) ? value * 2 : value
end
class Echo
  def call(value) = value
end
module Policy
  def self.check(value) = value + 1
end
module First
  def self.pass(value) = value
end
module Second
  def self.pass(value) = value
end
module Third
  def self.pass(value) = value + 1
end
def either(flag, value)
  checker = First.method(:pass)
  checker = Second.method(:pass) if flag
  checker.call(value)
end
values = [1.5, "s", 2]
[Doubler.new, Echo.new].each { |handler| p handler.call(values[0]), handler.call(values[2]) }
rows = [[1, 2], {2 => "a"}, "str"]
p rows[0][values[2]], rows[1][values[2]], rows[2][values[2]]
p Policy.check(0), Policy.method(:check).call(4)
p First.pass(0), Second.pass(0), Third.pass(0)
third = Third.method(:pass)
p either(false, values[0]), either(true, values[1]), third.call(5)
