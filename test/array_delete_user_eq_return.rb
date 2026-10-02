class Needle
  def ==(other)
    other == :hit
  end
end

a = [Needle.new]
results = [a.include?(:hit), a.index(:hit), a.count(:hit), a.rindex(:hit)]
removed = a.delete(:hit)
p [*results, removed.is_a?(Needle), a.empty?]

first = Needle.new
last = Needle.new
a = [first, :keep, last, :tail]
removed = a.delete(:hit)
p [removed.equal?(last), a]
p a.delete(:missing)
p a.delete(:missing) { |key| [key, :fallback] }

class CollectingNeedle
  def ==(other)
    GC.start
    other == :hit
  end
end
a = [CollectingNeedle.new, :keep, CollectingNeedle.new]
removed = a.delete(:hit)
GC.start
p [removed.is_a?(CollectingNeedle), a]
