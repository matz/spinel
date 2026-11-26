# A Class.new(parent) without a block, evaluated in a loop, makes its own
# subclass at each evaluation.
class NoBlockLoopBase; end
i = 0
while i < 2
  klass = Class.new(NoBlockLoopBase)
  p klass.superclass == NoBlockLoopBase
  i += 1
end
