# A capture on main meets the includes of Object and of main in the order they ran: a module a block includes
# at the top level (`1.times { include B }`) came after the one `class Object` includes, so B answers.
module A
  def f = "A#f"
end
module B
  def f = "B#f"
end
class Object
  include A
end
1.times { include B }
p method(:f).call
