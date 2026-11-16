# public_send of a writer's name on a union beside a class's own `def x=`:
# another class's private attr_writer raises NoMethodError, as CRuby does;
# __send__ reaches it
class Ctl
  def request=(r)
    @request = r
  end
end
class Conn
  attr_reader :request
  private
  attr_writer :request
end
o = [Ctl.new, Conn.new][(ARGV[0] || 1).to_i]
begin
  p o.public_send(:request=, 5)
rescue NoMethodError => e
  p e.class
end
p o.__send__(:request=, 6)
p o.instance_variable_get(:@request)
