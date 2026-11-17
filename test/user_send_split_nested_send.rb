# A computed send among a split send's arguments or in its `&`: the
# __send__ arm's copy of them is split too.
class Sock
  def send(data) = data.size
end
class Conf
  attr_accessor :column
  def blk = proc { :b }
  def runb(&b) = b.call
end
c = Conf.new
pick = [c, Sock.new][ARGV.size]
inner = [c, Sock.new][ARGV.size]
pick.send("#{:column}=", inner.send("#{:column}"))
p c.column
p pick.send("runb".to_sym, &inner.send("blk".to_sym))
