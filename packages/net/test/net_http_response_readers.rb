# A response's headers read through each, each_name, each_key, each_value
# and to_hash, under downcased names and in the order they came, with a
# repeated header joined or kept as separate values. fetch answers a value,
# or for an absent header the block, the default or a KeyError.
# content_length is the declared length, or nil when none is declared.
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
replies = [
  "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\nX-One: 1\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok",
  "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nuntil the end",
]
t = Thread.new do
  replies.each do |reply|
    c = server.accept
    while (line = c.gets)
      break if line.strip.empty?
    end
    c.write(reply)
    c.close
  end
end

def run(port)
  res = Net::HTTP.new("127.0.0.1", port).get("/")
  pairs = []
  res.each { |k, v| pairs << [k, v] }
  p pairs
  names = []
  res.each_name { |k| names << k }
  p names
  keys = []
  res.each_key { |k| keys << k }
  p keys
  values = []
  res.each_value { |v| values << v }
  p values

  h = res.to_hash
  p h
  h["x-new"] = ["1"]
  p res["x-new"]

  p res.fetch("X-One")
  p res.fetch("set-cookie")
  p res.fetch("X-None", "default")
  p res.fetch("X-None") { |k| "no #{k}" }
  p res.fetch("X-None", nil)
  begin
    res.fetch("X-None")
  rescue KeyError => e
    p [e.class, e.message]
  end

  p res.content_length
  res = Net::HTTP.new("127.0.0.1", port).get("/")
  p [res.content_length, res.body]
end

run(port)
t.join
