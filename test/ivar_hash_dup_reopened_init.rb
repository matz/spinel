# A String-only Hash passed to `new` before an Integer-valued one, in a
# class that is reopened with a second initialize. The first initialize
# never runs.
class Message
  def initialize(input_data) = @data = input_data.dup

  def data
    hash = @data.dup
    hash.merge!("ack-nonce" => 7)
    hash
  end
end

class Message
  def initialize(d)
    @data = d.dup
  end
end

p Message.new({"note" => "joined"}).data
p Message.new({"pong-nonce" => 3}).data

# the second initialize calls super
class Reply
  def initialize(input_data) = @data = input_data.dup

  def data
    hash = @data.dup
    hash.merge!("ack-nonce" => 8)
    hash
  end
end

class Reply
  def initialize(d)
    super()
    @data = d.dup
  end
end

p Reply.new({"note" => "left"}).data
p Reply.new({"pong-nonce" => 4}).data
