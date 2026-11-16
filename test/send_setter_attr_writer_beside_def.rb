# A writer's name sent to a union of two classes, one defining `def x=`, the
# other an attr_accessor: the attr_writer's arm stores and answers the value,
# the method's arm answers the method's own value
class Ctl
  def request=(r)
    @request = r
    :ctl_ret
  end
end
class Conn
  attr_accessor :request
end
o = [Ctl.new, Conn.new][(ARGV[1] || 1).to_i]
o2 = [Ctl.new, Conn.new][(ARGV[2] || 0).to_i]
h = { c: 3 }
p o.__send__((ARGV[0] || "request=").to_sym, h)
p o.instance_variable_get(:@request)
p o2.__send__((ARGV[0] || "request=").to_sym, **h)
p o2.instance_variable_get(:@request)
p o.public_send(:request=, 5)
p o.instance_variable_get(:@request)
