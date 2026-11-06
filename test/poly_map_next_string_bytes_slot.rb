# A `next` with a String in a map block over a poly receiver, when the map's
# result is kept as a typed String array: the block's value slot is the bytes
# face, and a `next` or a tail that reads as the handle is stored as its bytes
# (aws-eventstream's Encoder#encode_headers, every header a `next` or a pack).

class Message
  def initialize(options = {})
    @headers = options[:headers] || {}
  end
  attr_reader :headers
end

def packed(message)
  entries = message.headers.map do |key, value|
    encoded_key = [key.bytesize, key].pack('Ca*')
    next [encoded_key, "!"].pack('a*a*') if value == true
    [encoded_key, value.to_s].pack('a*a*')
  end
  entries.join
end

def mixed(message)
  entries = message.headers.map do |key, value|
    next key + "!" if value == true
    [key, value.to_s].pack('a*a*')
  end
  entries
end

def tails(message)
  entries = message.headers.map do |key, value|
    next [key].pack('a*') if value == true
    if value.is_a?(String)
      key + value
    else
      [key, value.to_s].pack('a*a*')
    end
  end
  entries
end

message = Message.new(headers: { 'a' => true, 'b' => 'xy', 'c' => 5 })
p packed(message).bytes
p mixed(message)
p tails(message)
p packed(Message.new).bytes
