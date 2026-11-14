# The parameters of an instance_eval / instance_exec block run on its own
# receiver, a nested one's included: a default reads the inner receiver's
# ivar, not the outer one's.
class A; def initialize = (@a = 1); end
class B; def initialize = (@a = 3); end
class S; def initialize = (@a = "b"); end

# a literal block, a proc written inline, a proc held in a local
p A.new.instance_eval { [@a, B.new.instance_exec { |q = @a| [q, @a] }] }
p A.new.instance_eval { [@a, B.new.instance_exec(&proc { |q = @a| [q, @a] })] }
p A.new.instance_eval { pr = proc { |q = @a| [q, @a] }; [@a, B.new.instance_exec(&pr)] }

# the inner receiver's ivar is a String
p A.new.instance_eval { [@a, S.new.instance_exec { |q = @a| [q, @a] }] }

# an argument given: the default is not run
p A.new.instance_eval { [@a, B.new.instance_exec(9) { |q = @a| [q, @a] }] }

# at the top level, no outer block
p B.new.instance_exec(&proc { |q = @a| [q, @a] })
p B.new.instance_exec { |q = @a| [q, @a] }

# a block forwarded to two receiver classes: its default reads each class's
# own ivar, nil for the class without it
class FA; def initialize = (@a = 1); end
class FB; def initialize = (@b = 2); end
def fwd(o, &b) = o.instance_exec(&b)
def both(&b) = [fwd(FA.new, &b), fwd(FB.new, &b)]
p both { |q = @a| [q, @b] }
p both { |q = 5| [q, @b] }
