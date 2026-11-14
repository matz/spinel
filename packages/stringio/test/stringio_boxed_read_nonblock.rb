require "stringio"

# The Hash keeps the receiver boxed. StringIO's Ruby method must run
# before the builtin IO output-buffer path checks the receiver's class.
io = { "io" => StringIO.new("abc") }["io"]
buffer = +"old"
p io.read_nonblock(2, buffer, exception: false), buffer
p io.read_nonblock(3, nil, exception: false)
p io.read_nonblock(1, buffer, exception: false), buffer
p io.read_nonblock(0, buffer), buffer
begin
  io.read_nonblock(1, buffer)
rescue EOFError => error
  puts error.class.name
end
p buffer
io.rewind
p io.read_nonblock(3, +"", exception: false)

begin
  io.read_nonblock(1, nil, exception: true)
rescue EOFError => error
  puts error.class.name
end

begin
  io.read_nonblock(-1, nil)
rescue ArgumentError => error
  puts error.class.name
end
begin
  io.read_nonblock(0, "frozen".freeze)
rescue FrozenError => error
  puts error.class.name
end

[-1, 0, 1].each do |length|
  frozen_stream = { "io" => StringIO.new("x").freeze }["io"]
  begin
    frozen_stream.read_nonblock(length, +"", exception: false)
  rescue FrozenError => error
    puts error.class.name
  end
end

copy = { "io" => StringIO.new("copy") }["io"]
other = copy
p other.read_nonblock(4, +"")
selected = ARGV.empty? ? { "io" => StringIO.new("left") }["io"] : nil
p selected.read_nonblock(4, +"", exception: false)

missing = { "io" => StringIO.new("unused") }["missing"]
begin
  missing.read_nonblock(1, +"")
rescue NoMethodError => error
  puts error.class.name
end
