# A Class.new(parent) without a block, evaluated in a block, makes its own
# subclass at each evaluation.
class NoBlockIteratorBase; end
made = []
2.times do
  klass = Class.new(NoBlockIteratorBase)
  p klass.superclass == NoBlockIteratorBase
  made << klass
end
p made[0].equal?(made[1]), made.uniq.size
