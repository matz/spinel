# A prepend that may not run guards its copies like any later prepend:
# a call made before it runs reaches the class's own method, and one made
# after it reaches the module's version live then, or the class's method
# again where the prepend did not run.
module V
  def a = [:v, super]
end

class Taken
  def a = :taken
end

class Untaken
  def a = :untaken
end

class Twice
  def a = :twice
end

class Inside
  def a = :inside
end

p Taken.new.a
p Untaken.new.a
if ARGV.empty?
  class Taken; prepend V; end
  class Twice; prepend V; end
end
unless ARGV.empty?
  class Untaken; prepend V; end
end
p Taken.new.a
p Untaken.new.a
p Twice.new.a
# a call in the reopening right after its prepend runs the module's method
class Inside
  prepend V
  p new.a
end
# a certain prepend of a module a prepend that may not have run brought
class Twice; prepend V; end
p Twice.new.a

module V
  def a = [:v2, super]
end

p Taken.new.a
p Untaken.new.a
p Twice.new.a
