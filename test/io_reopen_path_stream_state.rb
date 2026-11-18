# spinel: gc-stress
require "tmpdir"
# IO#reopen(path) on an open handle keeps its number and its stream, and the
# stream forgets the old file: what it read ahead, an end of file it saw,
# its offset; the descriptor is close-on-exec, as File.open gives it. What it buffered for the old file is written before the new
# one is opened, so a reopen onto its own path truncates after the write.
# A mode that changes the side of the stream takes effect; a closed handle
# gets a fresh stream.
Dir.mktmpdir("sp_reopen") do |d|
  a = File.join(d, "a")
  b = File.join(d, "b")
  File.write(a, "line1\nline2\n")
  File.write(b, "other\n")
  f = File.open(a)
  puts f.gets
  n = f.fileno
  f.reopen(b)
  puts f.fileno == n
  puts f.close_on_exec?
  puts f.read
  f.reopen(a)
  puts f.gets
  f.close
  w = File.open(a)
  w.reopen(b, "a")
  w.write("more\n")
  w.close
  puts File.read(b)
  c = File.open(a)
  c.close
  c.reopen(b, "r")
  puts c.read.lines.size
  c.close
  s = File.join(d, "s")
  o = File.open(s, "w")
  o.write("old")
  o.reopen(s, "w")
  o.close
  puts File.read(s).empty?
  junk = Array.new(50) { |k| "junk #{k}" }
  puts "ok #{junk.size}"
end
