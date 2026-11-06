# `extend self`, read, then a singleton def on the module
module M
  extend self
  def v = 1
end
puts M.v
def M.v = 2
puts M.v
