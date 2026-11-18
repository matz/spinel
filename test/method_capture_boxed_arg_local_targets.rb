# A Method local that takes a second Method through a write that is no
# plain assignment of a method(:sym): nested and splatted multiple
# assignment, `for`, `rescue =>`, a pattern, `||=`, a write inside a block
# or a lambda, and a block or lambda parameter of the same name. Each call
# runs the Method the local holds, with the boxed argument of
# method_capture_boxed_arg.rb (method_capture_boxed_arg_hidden_write.rb
# holds the forms that answered a wrong value).
module Destination
  def self.first(value) = [:first, value]
  def self.second(value) = [:second, value]
end
def factory = Destination.method(:second)
class Carrier < StandardError
  def call(value) = factory.call(value)
end
def nested(value)
  chosen = Destination.method(:first)
  (spare, chosen), other = [[0, factory], 1]
  chosen.call(value)
end
def splatted(value)
  chosen = Destination.method(:first)
  *spare, chosen = [0, 1, factory]
  chosen.call(value)
end
def looped(value)
  chosen = Destination.method(:first)
  for chosen in [factory]
  end
  chosen.call(value)
end
def rescued(value)
  chosen = Destination.method(:first)
  begin
    raise Carrier
  rescue Carrier => chosen
  end
  chosen.call(value)
end
def matched(value)
  chosen = Destination.method(:first)
  case [factory]
  in [chosen]
  end
  chosen.call(value)
end
def rightward(value)
  chosen = Destination.method(:first)
  factory => chosen
  chosen.call(value)
end
def in_block(value)
  chosen = Destination.method(:first)
  1.times { chosen = factory }
  chosen.call(value)
end
def in_lambda(value)
  chosen = Destination.method(:first)
  -> { chosen = factory }.call
  chosen.call(value)
end
def defaulted(value, flag)
  chosen = Destination.method(:first)
  chosen = nil if flag
  chosen ||= factory
  chosen.call(value)
end
def block_parameter(value)
  chosen = Destination.method(:first)
  [factory].map { |chosen| chosen.call(value) } << chosen.call(0)
end
def lambda_parameter(value)
  chosen = Destination.method(:first)
  inner = ->(chosen, given) { chosen.call(given) }
  [inner.call(factory, value), chosen.call(0)]
end
p Destination.first(0), Destination.second(0)
[1.5, "str", nil, [2, 3], 4].each do |value|
  p nested(value), splatted(value), looped(value), rescued(value)
  p matched(value), rightward(value), in_block(value), in_lambda(value)
  p defaulted(value, true), defaulted(value, false), block_parameter(value), lambda_parameter(value)
end
