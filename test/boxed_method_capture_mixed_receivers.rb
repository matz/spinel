# spinel: int64 -- assumes a 64-bit Integer (1.size answers 8); not run on a 32-bit target
module Reader
  def size = 7
end

class Peer
  def size = 81
end

text = +"abc"
class String
  prepend Reader if ENV["SPINEL_TEST_METHOD_PREPEND"] == "1"
end

receivers = [text, Peer.new, [1, 2, 3, 4], {key: 1}, (1..5), 1]
p receivers.map { |receiver| receiver.method(:size).call }
class String
  def size = 19
end
p receivers.map { |receiver| receiver.method(:size).call }
