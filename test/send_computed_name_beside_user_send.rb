# A class's own `send` (a WebSocket's `send(data)`) answers a send on its
# instances; on any other receiver `send` is Object's, which calls the
# method the computed name names.
module Net
  class WS
    def send(data) = "sent #{data}"
  end
end
module Sends
  def send(*args) = "own #{args.size}"
end
class Pipe
  include Sends
end
class Conf
  def column = @c
  def column=(v)
    @c = v
  end
end
def set(o, key, v) = o.send("#{key}=", v)
objs = [Net::WS.new, Conf.new, Pipe.new]
conf = objs[ARGV.size + 1]
p set(conf, :column, 7)
p conf.column
msg = ARGV[0] || "hello"
p objs[ARGV.size].send(msg)
p objs[ARGV.size + 2].send(msg, 1)
p [Conf.new, 3].map { |o| o.send(ARGV[1] || "class") }
def fwd(o, *args) = o.send(*args)
fwd(conf, :column=, 9)
p conf.column
class Strict
  def self.===(_) = false
  def send(x) = "strict #{x}"
end
s = [Strict.new, conf][ARGV.size]
p s.send(msg)
none = [nil, conf][ARGV.size]
p none&.send(msg)
r = objs[ARGV.size]
p r.send((r = conf; "column"))
