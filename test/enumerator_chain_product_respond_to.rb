# Enumerator::Chain and Enumerator::Product undefine next, peek,
# next_values, peek_values and feed. They answer the rest of an
# Enumerator's names, by a literal name and by one only the run time knows.
ch = [1, 2].each.chain([3])
pr = Enumerator.product([1, 2], [3])
en = [1, 2].each
[[ch, "c"], [pr, "p"], [en, "e"]].each { |o, t| p [t, o.respond_to?(:next), o.respond_to?("peek")] }
p [ch.respond_to?(:next), ch.respond_to?(:peek), ch.respond_to?(:next_values), ch.respond_to?(:peek_values), ch.respond_to?(:feed)]
p [pr.respond_to?(:next), pr.respond_to?(:feed), pr.respond_to?(:rewind), pr.respond_to?(:size)]
p [en.respond_to?(:next), en.respond_to?(:feed), en.respond_to?(:peek_values)]
puts "if-ch" if ch.respond_to?(:next)
puts "if-en" if en.respond_to?(:next)
puts(ch.respond_to?(:next) ? "t" : "f")
x = rand < 2 ? en : ch
p x.respond_to?(:next)
y = rand < 2 ? ch : en
p y.respond_to?(:next)
z = rand < 2 ? nil : en
p z.respond_to?(:next)
h = { a: en.respond_to?(:next), b: ch.respond_to?(:next) }
p h
def mk(n) = n > 0 ? [1].each.chain([2]) : [1].each
p mk(1).respond_to?(:peek), mk(0).respond_to?(:peek)
p [1].each.chain([2]).respond_to?(:next, true)
names = %w[next peek next_values peek_values rewind with_index each size map each_slice zzz]
names.each do |n|
  p [n, ch.respond_to?(n), pr.respond_to?(n), en.respond_to?(n)]
  p [n, ch.respond_to?(n.to_sym), pr.respond_to?(n.to_sym)]
end
[ch, pr, en, 1].each { |o| p [o.class, o.respond_to?("next"), o.respond_to?(:rewind)] }
