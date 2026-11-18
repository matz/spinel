# A captured method the call's arguments cannot enter is not its target: a
# one-argument call of a boxed receiver raises ArgumentError before a method
# of two parameters reads either, so both keep the sp_int lane their direct
# calls gave them. The one-parameter method beside it takes the boxed value.
module Pair
  def self.sum(a, b) = a + b
end
module One
  def self.echo(value) = value
end
def run(table, key, value) = table[key].call(value)
p Pair.sum(1, 2), One.echo(0)
pair = Pair.method(:sum)
table = {one: One.method(:echo), id: ->(value) { value }, pair: pair}
values = [1.5, "s", 2]
p run(table, :one, values[0]), run(table, :id, values[1]), pair.call(3, 4)
begin
  run(table, :pair, values[0])
rescue ArgumentError
  puts "ArgumentError"
end
