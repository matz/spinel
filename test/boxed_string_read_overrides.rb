# A boxed receiver can be a base String or a user object. A method
# installed on String owns the call instead of its original builtin.
class ReadPeer
  def delete(x) = "peer delete"
  def partition(x) = "peer partition"
  def split(x) = "peer split"
  def encode(x) = "peer encode"
  def [](x) = "peer index"
end

class String
  def delete(x) = "String delete"
  def partition(x) = "String partition"
  def split(x) = "String split"
  def encode(x) = "String encode"
  def [](x) = "String index"
end

def read(value)
  [value.delete("b"), value.partition("b"), value.split("b"),
   value.encode("UTF-8"), value[0]]
end

p read([+"abc", ReadPeer.new][0])
p read(ReadPeer.new)
