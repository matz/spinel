# The body a module_function copy runs calls a method that is not a copy,
# and the program replaces that method after the first call. The record
# gives the body a first run at the statement that reaches the copy, so the
# call in it meets the version live when it runs, not the last one defined:
# a bare copy calling a class method, a named copy calling one, and a bare
# copy calling an instance method of another class.
class K
  def self.v = 1
end
module U
  module_function
  def b = K.v
end
puts U.b
def K.v = 2
puts U.b

class L
  def self.v = 1
end
module V
  def b = L.v + 10
  module_function :b
end
puts V.b
def L.v = 2
puts V.b

class M
  def v = 1
end
module W
  module_function
  def b = M.new.v
end
puts W.b
class M
  def v = 2
end
puts W.b
