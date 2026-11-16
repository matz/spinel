# A dup that runs before an initialize_copy the program defines under a
# condition (taken) copies with Object's hook: the hook's own work does not
# happen until the def has run.
class Cond
  attr_accessor :a
  def initialize = @a = [1]
end
cond = Cond.new
p cond.dup.a.equal?(cond.a)
if ARGV.empty?
  class Cond
    def initialize_copy(o) = @a = o.a.dup
  end
end
p cond.dup.a.equal?(cond.a)
p cond.clone.a.equal?(cond.a)
