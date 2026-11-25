# A Hash passed to new reaches @data, where the walk of the Hash's origins
# meets a new it cannot follow beside the new calls it can.
require "set"

# a new on a constant no class has here, in a method nothing calls
module Outer
  module Incoming
    def join(data)
      Message.new(data, session: 1)
    end
  end
end

module Outer::Client
  class Message
    def initialize(input_data, session:)
      @session = session
      @data = input_data.dup
    end

    def data
      hash = @data.dup
      hash.merge!("from-old-session" => @session)
      hash
    end
  end
end

Outer::Client::Message.new({"note" => "ack", "ack-nonce" => 5}, session: 99)
source = {"note" => "command"}
copy = Outer::Client::Message.new(source, session: 2)
p copy.data["note"]
p copy.data

# a module method's bare new, through extend
class Built
  def initialize(input_data)
    @data = input_data.dup
  end

  def data
    hash = @data.dup
    hash.merge!("ack-nonce" => 8)
    hash
  end
end

module Factory
  def build(h) = new(h)
end
Built.extend(Factory)

p Built.build({"note" => "joined"}).data
p Built.build({"pong-nonce" => 3}).data

# a splat of a local Array
class Spread
  def initialize(input_data)
    @data = input_data.dup
  end

  def data
    hash = @data.dup
    hash.merge!("ack-nonce" => 9)
    hash
  end
end

p Spread.new({"note" => "left"}).data
args = [{"pong-nonce" => 4}]
p Spread.new(*args).data
