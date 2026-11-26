# A Class.new(parent) without a block, evaluated in a block, makes its own
# subclass at each evaluation.
class NoBlockIteratorBase; end
2.times do
  klass = Class.new(NoBlockIteratorBase)
  p klass.superclass == NoBlockIteratorBase
end
