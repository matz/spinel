# A send whose name an instance variable holds, where a write stores a name
# built at run time (#12025): the ivar holds a name no literal spells, so
# the send takes the arms of every method, as a parameter handed one does
# (#8592). The ivar is written from initialize's parameter, which the `new`
# calls hand their argument; from another ivar; and from a local.
class Task
  def initialize(name) = @name = name
  def run = send(@name)
  def copy_to(o) = (@other = @name; o)
  def run_other = public_send(@other)
  def step_one = :one
  def step_two = :two
end
p Task.new(["step_", "one"].join).run
t = Task.new("step_" + "two")
t.copy_to(nil)
p t.run_other

class Plan
  def set(n)
    s = "step_#{n}"
    @next = s
    self
  end
  def go = send(@next)
  def step_a = :a
end
p Plan.new.set("a").go
begin
  Task.new(%w[step_ three].join).run
rescue NoMethodError => e
  p e.message
end
