# A class body closed early, a file's worth of definitions, then the class
# reopened to prepend a module: the top-level statements between them only
# answer a value (a literal, a constant), so no method body defined ahead of
# the prepend can run there, and a call in one reaches the prepended method.
class Store
  def clear = :store
end

class Reporter
  def initialize(s) = @s = s
  def clear_context = @s.clear
end

class Other
  def clear = :other
end

class MemoryStore < Store
end

false
nil
Store

module LocalCache
  def clear = [:local, super]
end

:loaded

class MemoryStore
  prepend LocalCache
end

p Reporter.new(MemoryStore.new).clear_context
p Reporter.new(Other.new).clear_context
p Reporter.new(Store.new).clear_context
