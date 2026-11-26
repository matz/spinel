# a receiverless send of a writer's name reaches an attr writer, as
# `self.send` does (net-ldap's SnmpPdu: `send :request_id=, data[0].to_i`)
class Pdu
  attr_accessor :request_id
  attr_writer :error_index
  attr_reader :error_index
  def def_writer=(v)
    @dw = v * 2
  end
  attr_reader :dw

  def parse(data)
    a = send :request_id=, data[0].to_i
    __send__(:error_index=, data[1].to_i)
    public_send("def_writer=", 4)
    send(:hidden=, 7)
    [a, @hidden]
  end

  def try_public = public_send(:hidden=, 8)

  private

  attr_writer :hidden
end

n = Pdu.new
p n.parse(["16170", "3"])
p [n.request_id, n.error_index, n.dw]
begin
  n.try_public
rescue NoMethodError => e
  p e.class
end

# from a superclass, to a subclass's attr writer and to another's def writer
class Gauge
  def write(v) = send(:level=, v)
end
class Dial < Gauge
  attr_accessor :level
end
class Knob < Gauge
  attr_reader :level
  def level=(x)
    @level = x * 2
  end
end
d = Dial.new
d.write(5)
k = Knob.new
k.write(5)
p [d.level, k.level]

# a writer name past ASCII
class Cafe
  attr_accessor :café
  def set(v) = send(:café=, v)
end
c = Cafe.new
c.set(3)
p c.café
