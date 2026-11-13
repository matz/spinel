# A method no node names is still reached by a Symbol made at run time:
# a block or a proc from it calls the method it names.
class T
  def secret = "s"
  def last_one = :kept
end
sym = ["sec", "ret"].join.to_sym
p [T.new].map(&sym)
p sym.to_proc.call(T.new)
