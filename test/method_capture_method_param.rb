# A captured method whose parameter a direct call typed Method keeps that
# Method when a boxed argument reaches some other captured method through
# a container call. Boxed, it lost super_method and original_name, and
# the call raised NoMethodError.
class Item
  def read = 1
end
module Spare
  def self.root?(m) = m.super_method.nil?
  def self.label(m) = m.original_name
end
module Policy
  def self.echo(v) = v
end
def run(box, x) = box[:cb].call(x)
p Policy.echo(0)
values = [3, 1.5, "s"]
p run({cb: Policy.method(:echo)}, values[0])
p run({cb: Policy.method(:echo)}, values[1])
p Spare.root?(Item.new.method(:read))
p Spare.label(Item.new.method(:read))
p Spare.method(:root?).name
p Spare.method(:label).name
