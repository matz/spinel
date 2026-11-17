# A computed send as a split send's name or receiver: it is split after
# the outer one, from where the outer split moved it.
class Sock
  def send(data) = data.size
end
class Conf
  def key = :column
  def column = "col"
  def me = :itself
end
c = Conf.new
pick = [c, Sock.new][ARGV.size]
n = :key
p pick.send(pick.send(n))
p pick.send(c.me).send(pick.send(n))
p pick.send(c.me).send(c.me).send(:column)
