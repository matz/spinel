# An Enumerable method on a receiver known only at run time walks what the
# object's #each yields, not what a #to_a of the class's own answers.
class Twofold
  include Enumerable
  def each = (yield 1; yield 2; self)
  def to_a = [9]
end

t = [1].size > 3 ? 5 : Twofold.new
p t.to_a

# the spliced map walk reads the elements once
p t.map { |i| i > 1 }
p t.collect { |i| i + 1 }, t.flat_map { |i| [i, i] }

# the reductions
p t.count, t.include?(2), t.include?(9)
p t.first, t.first(1), t.min, t.max, t.minmax, t.sum
p t.sort, t.sort_by { |i| -i }, t.uniq, t.tally
p t.inject { |a, b| a + b }, t.reduce(0) { |a, b| a + b }
p t.take(5), t.drop(1), t.find_index(2), t.zip([7, 8])

# an Enumerator over the object
p t.each_with_index.to_a, t.each_slice(1).to_a, t.each_cons(2).to_a
p t.reverse_each.to_a
p t.lazy.map { |i| i * 2 }.to_a

# #each runs once for one walk
class Loud
  include Enumerable
  def each
    puts "each"
    yield 1
    yield 2
    self
  end
end

l = [1].size > 3 ? 5 : Loud.new
p l.map { |i| i + 1 }
