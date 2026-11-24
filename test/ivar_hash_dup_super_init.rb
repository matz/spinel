# A String-only Hash passed to `new` before an Integer-valued one, in a
# program where another class's initialize calls super.
class Message
  def initialize(input_data)
    @data = input_data.dup
  end

  def data
    hash = @data.dup
    hash.merge!("ack-nonce" => 7)
    hash
  end
end

class Sub
  def initialize(a)
    super()
  end
end

p Message.new({"note" => "joined"}).data
p Message.new({"pong-nonce" => 3}).data

# super in the initialize the Hashes are passed to
class Base
  def initialize(tag)
    @tag = tag
  end
end

class Tagged < Base
  def initialize(input_data)
    super("m")
    @data = input_data.dup
  end

  def data
    hash = @data.dup
    hash.merge!("ack-nonce" => 7)
    hash
  end
end

p Tagged.new({"note" => "left"}).data
p Tagged.new({"pong-nonce" => 4}).data
