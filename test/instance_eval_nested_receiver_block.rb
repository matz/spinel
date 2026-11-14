# An instance_eval block nested in another runs on its own receiver: the
# outer receiver's class does not type the inner block's ivars.
class A; def initialize = (@w = 1); end
class B; def initialize = (@w = 2; @v = :b); end
class C; def initialize = (@v = 3); end

class W
  def run(o, b) = o.instance_eval { [@v, b.instance_eval { @w }] }
  def pair(a, b) = a.instance_eval { [@w, b.instance_eval { [@w, @v] }] }
  def bump(a, b) = a.instance_eval { b.instance_eval { @w = @w + 10 }; @w }
end

p [A.new, C.new].map { |o| W.new.run(o, B.new) }
p W.new.pair(A.new, B.new)
b = B.new
p W.new.bump(A.new, b)
p b.instance_eval { @w }
p A.new.instance_eval { [@w, B.new.instance_eval { [@w, @v] }] }

# ... a proc handed to the nested call with `&` too, a local's included.
class A2; def initialize = (@w = 5); end
class D; def initialize = (@b = 2); end
d = D.new
p([A.new, A2.new].map { |o| o.instance_eval { pr = proc { [@b, @w] }; [@w, d.instance_exec(&pr)] } })
p([A.new, A2.new].map { |o| o.instance_eval { [@w, d.instance_exec(&proc { [@b, @w] })] } })
