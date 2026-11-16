# The writer arms beside a method dispatch, with a String and a Class value in
# the union too: each receiver answers as in CRuby
class Ctl
  def request=(r)
    @request = r
    :ctl_ret
  end
end
class Conn
  attr_accessor :request
end
objs = [Ctl.new, Conn.new, "s", Ctl]
[0, 1, 2, 3].each do |i|
  o = objs[(ARGV[i] || i).to_i]
  begin
    p o.public_send(:request=, { c: i })
    p o.instance_variable_get(:@request)
  rescue NoMethodError => e
    p e.class
  end
end
