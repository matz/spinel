# An ivar assigned the Hashes of two `new` calls, of different value kinds,
# is boxed: a store through it, or through a copy or a local alias of it,
# widens the Hashes the calls pass.
class Message
  def initialize(data)
    @data = data.dup
  end

  def data
    h = @data.dup
    h.merge!("ack-nonce" => 7)
    h
  end
end

p Message.new({"pong-nonce" => 3}).data
p Message.new({"note" => "joined"}).data

class Direct
  def initialize(data)
    @data = data
  end

  def put
    @data["k"] = 7
    @data
  end

  def upd
    h = @data
    h.update("u" => 1.5)
    h
  end

  def two
    @data.merge!({"k" => 7}, {"z" => :s})
    @data
  end
end

p Direct.new({"a" => 3}).put
p Direct.new({"b" => "x"}).put
p Direct.new({"a" => 3}).upd
p Direct.new({"b" => "x"}).upd
p Direct.new({"a" => 3}).two
p Direct.new({"b" => "x"}).two

def merged(d)
  h = d
  h.merge!("k" => 7)
  h
end

p merged({"a" => 3})
p merged({"b" => "x"})

# brace-free pairs are the Hash a positional parameter takes
p Message.new("pong-nonce" => 4).data
p Message.new("note" => "left").data
p merged("a" => 3)
p merged("b" => "x")

def opt(n, d = {"z" => 1})
  h = d
  h["k"] = 7
  h
end

p opt(1, "a" => 3)
p opt(2, "b" => "x")
