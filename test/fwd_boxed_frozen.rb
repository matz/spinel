# frozen_string_literal: true
# Frozen sources may cross a preserving boxed-array store, without adding
# any new shared-handle promotion. Identity and FrozenError both survive.
class FrozenCollector
  def initialize = @items = []
  def accept(value); store(value); nil; end
  def store(value); @items.push(value); nil; end
  def gather(values); values.to_a.each { |value| store(value) }; nil; end
  def at(index) = @items[index]
end

collector = FrozenCollector.new
collector.accept(7)
text = "alpha"
collector.accept(text)
p collector.at(1).equal?(text)
p collector.at(1).frozen?
begin
  collector.at(1) << "!"
rescue FrozenError
  puts "frozen"
end
collector.gather(["beta", "gamma"])
p collector.at(2), collector.at(3), text
