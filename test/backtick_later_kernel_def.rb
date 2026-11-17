p 5.instance_eval { `echo a` }
p "s".instance_eval { `echo b` }
module Kernel
  def `(c) = "K #{c}"
end
p 5.instance_eval { `echo c` }
