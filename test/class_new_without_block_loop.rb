# A Class.new(parent) without a block, evaluated in a loop, makes its own
# subclass at each evaluation.
class NoBlockLoopBase; end
made = []
i = 0
while i < 2
  klass = Class.new(NoBlockLoopBase)
  p klass.superclass == NoBlockLoopBase
  made << klass
  i += 1
end
p made[0].equal?(made[1]), made.uniq.size
