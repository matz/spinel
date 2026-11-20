# A request's headers hold separate values: add_field adds one after those
# a header holds, get_fields answers them and #[] joins them. to_hash reads
# every header that way, fetch answers a value, or for an absent header the
# block, the default or a KeyError, and delete removes one. A header with
# several values goes out on one line.
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  c = server.accept
  seen = []
  while (line = c.gets)
    break if line.strip.empty?
    seen << line.strip if line.downcase.start_with?("x-")
  end
  out = seen.sort.join(" | ")
  c.write("HTTP/1.1 200 OK\r\nContent-Length: #{out.bytesize}\r\nConnection: close\r\n\r\n#{out}")
  c.close
end

def run(port)
  req = Net::HTTP::Get.new("/", "X-A" => "1")
  p req.get_fields("x-a")
  p req.get_fields("X-None")
  req.add_field("X-A", "2")
  p req.get_fields("X-A")
  p req["x-a"]
  req.add_field("X-B", "3")
  p req.get_fields("x-b")
  req.add_field("X-B", %w[4 5])
  p req.get_fields("x-b")
  req["X-C"] = %w[6 7]
  p req.get_fields("x-c")
  p req["x-c"]
  req["X-C"] = "8"
  p req.get_fields("x-c")
  begin
    req.add_field("X-A", "bad\r\nvalue")
  rescue ArgumentError => e
    p e.class
  end
  p req.get_fields("x-a")

  values = req.get_fields("x-a")
  values << "9"
  p req.get_fields("x-a")

  p req.to_hash
  h = req.to_hash
  h["x-new"] = ["1"]
  p req["x-new"]

  p req.fetch("X-A")
  p req.fetch("X-None", "default")
  p req.fetch("X-None") { |k| "no #{k}" }
  p req.fetch("X-None", nil)
  begin
    req.fetch("X-None")
  rescue KeyError => e
    p [e.class, e.message]
  end

  p req.delete("X-C")
  p req.delete("X-C")
  p [req["x-c"], req.key?("x-c"), req.get_fields("x-c")]
  p req.delete("Accept")
  p req.to_hash.keys

  p Net::HTTP.new("127.0.0.1", port).request(req).body
end

run(port)
t.join
