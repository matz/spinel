# A name an attr form makes that a builtin answers too, by running a block
# the program stored (Enumerator#count runs the Enumerator's block), still
# may define: `run(V)` runs V's block, which redefines Macros#column, so the
# macro after it waits for it, refused.
module Macros
  def column(name)
    define_method(name) { "col #{name}" }
  end
end
class Item
  attr_reader :count
end
V = Enumerator.new { |y| Macros.class_eval { def column(n) = define_method(n) { "r #{n}" } }; y << 1 }
def run(x) = x.count
class Base
  extend Macros
  run(V)
  column :title
end
p Base.new.title
