# spinel: not-cruby -- Uses net-protocol 0.4.0; CRuby 4.0 bundles 0.2.2.
# Expected output comes from CRuby with ruby/net-protocol v0.4.0 on its load path.
require "net/protocol"

def with_input(data)
  reader, writer = IO.pipe
  writer.write(data)
  writer.close
  yield Net::BufferedIO.new(reader)
ensure
  reader.close
end

with_input("abc\ndefgh\n") do |io|
  p io.readuntil("\n", limit: 4)
  begin
    io.readuntil("\n", false, limit: 5)
  rescue => error
    p error.class
    p error.is_a?(Net::ProtocolError)
  end
  p io.readuntil("\n", limit: 6)
end

# The keyword must not become the optional positional ignore_eof argument.
with_input("abc\n") do |io|
  begin
    io.readuntil("\n", limit: 2)
  rescue => error
    p error.class
  end
  p io.readuntil("\n", {limit: 2})
end
with_input("abc\n") do |io|
  options = {limit: 2}
  begin
    io.readuntil("\n", **options)
  rescue => error
    p error.class
  end
  p io.readuntil("\n", false, limit: nil)
end
with_input("abc\n") do |io|
  begin
    io.readuntil("\n", {limit: 10}, limit: 2)
  rescue => error
    p error.class
  end
  p io.readuntil("\n")
end

class LimitValue
  def to_int
    raise "must not convert"
  end

  def to_s
    raise "must not format"
  end

  def is_a?(klass)
    true
  end
end

[0, -1, false, 4.5, "4", LimitValue.new].each do |limit|
  with_input("abc\n") do |io|
    begin
      io.readuntil("\n", limit: limit)
    rescue => error
      p error.class
    end
    p io.readuntil("\n")
  end
end

with_input("abc") do |io|
  p io.readuntil("\n", true, limit: 3)
end
with_input("abcd") do |io|
  begin
    io.readuntil("\n", true, limit: 3)
  rescue => error
    p error.class
  end
  p io.readuntil("\n", true)
end
with_input("abc") do |io|
  begin
    io.readuntil("\n", false, limit: 3)
  rescue => error
    p error.class
  end
  p io.readuntil("\n", {limit: 1})
end
with_input("\n") do |io|
  p io.readuntil("\n", limit: 1)
end
with_input("\xC3\xA9\n".b) do |io|
  begin
    io.readuntil("\n", limit: 2)
  rescue => error
    p error.class
  end
  p io.readuntil("\n", limit: 3).bytes
end
