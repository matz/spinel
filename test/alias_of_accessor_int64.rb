# spinel: int64
# An alias of an accessor's writer stores what the writer stores, past 2^31,
# and the alias of its reader reads it, whatever a later def of the name does.
class K
  attr_accessor :n
  alias_method :set_n, :n=
  alias get_n n
  def n = -1
end
k = K.new
k.set_n(2**40 + 3)
p k.get_n
p k.get_n.class
p k.n
