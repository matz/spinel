# A def in an instance_eval block defines a singleton method of the
# receiver, beside the block's other statements, which run in order; an
# instance_exec evaluates its argument and hands it to the block.
class Shape; end
Shape.instance_eval do
  puts "setup"
  def sides = 4
  puts "after"
end
p Shape.sides
$log = []
def note(x) = ($log << x; x)
Shape.instance_exec(note(:arg)) { |a| def corners = 4; p a }
p Shape.corners, $log
module Tool; end
Tool.instance_eval { def name_of = :tool }
p Tool.name_of
