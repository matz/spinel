# A block nested in an instance_eval whose receiver may be no object keeps
# its own receiver's ivars: the outer receiver being an Integer (where its
# ivars read nil) does not make the inner receiver's nil too.
class A; def initialize = (@a = 1); end
class A2; def initialize = (@a = :s); end
class B; def initialize = (@a = 3); end

# a proc held in a local, called on the inner receiver
p([A.new, A2.new, 5].map { |o| o.instance_eval { pr = proc { @a }; [@a, B.new.instance_exec(&pr)] } })
p([A.new, 5].map { |o| o.instance_eval { pr = proc { @a }; [@a, B.new.instance_exec(&pr)] } })
p([A.new, A2.new].map { |o| o.instance_eval { pr = proc { @a }; [@a, B.new.instance_exec(&pr)] } })

# a literal block
p([A.new, A2.new, 5].map { |o| o.instance_eval { [@a, B.new.instance_exec { @a }] } })
p([A.new, 5].map { |o| o.instance_eval { [@a, B.new.instance_exec { |q = @a| [q, @a] }] } })

# an ivar written only in a nested block (its own receiver's) does not stop
# the outer body from running on the non-object receiver of a mixed set
class Z; def initialize = (@z = 0); attr_reader :z; end
p([A.new, B.new, 5].map { |o| o.instance_eval { s = inspect; c = Z.new; c.instance_eval { @z = 3 }; [@a, s.size > 0, c.z] } })
