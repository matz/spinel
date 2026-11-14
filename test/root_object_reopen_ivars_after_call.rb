# A call on a builtin value that runs ahead of a reopening of Object that
# defines the method's name is the builtin's, at the top level and in a
# method called once the reopening has run; a call after it is the
# reopening's (`5.instance_variables` and the ivar reads and writes).
x = [1, 2]
x.instance_variable_set(:@a, 1) rescue p $!.class
p((x.instance_variable_get(:@a) rescue $!.class))
p 5.instance_variables
class Object
  def instance_variables = :obj_ivs
end
p 5.instance_variables
p x.instance_variables
def lst(o) = o.methods.size > 0
p lst(5)
def later = 7.instance_variables
p later
