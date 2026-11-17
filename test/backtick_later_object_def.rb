p 5.instance_eval { `echo a` }
class Object
  def `(c) = "O #{c}"
end
p 5.instance_eval { `echo c` }
