# A method the program adds to Comparable or Enumerable may call an Object
# method such as to_s or inspect with no receiver. The call goes to the
# receiver (self), not to the top-level object, even with a top-level def
# of the same name.
def to_s = "main"
def inspect = "MAIN"

module Comparable
  def bang = to_s + "!"
  def shown = "<" + inspect + ">"
  def facts = [self.class, nil?, is_a?(Integer), respond_to?(:abs)]
  def same = [itself, send(:to_s), hash == self.hash]
end

module Enumerable
  def shown = inspect + "#"
  def facts = [self.class, is_a?(Array), respond_to?(:each)]
  def counted = to_a.size + count
end

p 3.bang, 2.5.bang, "x".bang, :y.bang
p 3.shown, 2.5.shown, "x".shown
p 3.facts, 2.5.facts, :y.facts
p 3.same, 2.5.same
p [1, 2].shown, ({ a: 1 }).shown, (1..2).shown
p [1].facts, ({ a: 1 }).facts, (1..2).facts
p [1, 2].counted, (1..3).counted
