p "s".instance_eval { `echo a` }
class String
  def `(c) = "S #{c}"
end
p "s".instance_eval { `echo b` }
