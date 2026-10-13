# An attribute reader whose field the rule widens to a box only after the
# route was recorded (here the Array `names[0]` comes from holds its
# elements boxed once a change through another name reaches them) hands
# that box into a Hash literal: the value is the String the field holds,
# not a copy. With a receiver (a Rails app's `url_options = { host:
# request.host }`, #8328) and, here, without one, inside the class.
class Domain
  def initialize(name) = @name = name
  def qualify = (@name << ".com"; nil)
end

class Request
  attr_accessor :host

  def initialize
    @host = "localhost"
  end

  def options = { host: host }
end

module Current
  def self.url_options=(options)
    @url_options = options
  end

  def self.url_options = @url_options
end

def set_current(request)
  return nil if request.nil?
  protocol = +"http"
  protocol << "s"
  Current.url_options = {
    protocol: protocol,
    host: request.host,
  }
  nil
end

name = +"example"
Domain.new(name).qualify
names = [name]
request = Request.new
request.host = names[0]
set_current(nil)
set_current(request)
p Current.url_options
p Current.url_options[:host].equal?(request.host)
Current.url_options = request.options
p Current.url_options
p Current.url_options[:host].equal?(request.host)
