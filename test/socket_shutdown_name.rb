# BasicSocket#shutdown takes the direction as an Integer or as a Symbol or
# String name, with or without the SHUT_ prefix, on a typed socket and on
# one read back out of an Array.
require "socket"

[:WR, :SHUT_WR, "WR", "SHUT_WR"].each do |how|
  a, b = UNIXSocket.pair
  p a.shutdown(how)
  p b.read
end

a, b = UNIXSocket.pair
p a.shutdown(:RD)
a.write("x")
p b.read(1)

[:RDWR, Socket::SHUT_WR, "SHUT_RDWR"].each do |how|
  pair = [UNIXSocket.pair].map(&:first)
  pair.each { |s| p s.shutdown(how) }
end

[:FOO, "SHUT_X"].each do |how|
  a, b = UNIXSocket.pair
  begin
    a.shutdown(how)
  rescue SocketError => e
    puts "#{e.class}: #{e.message}"
  end
end
