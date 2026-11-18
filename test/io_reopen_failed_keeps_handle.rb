# spinel: gc-stress
require "tmpdir"
# IO#reopen with a path that cannot be opened raises and leaves the handle
# as it was, as CRuby's does, and so does one with a mode that is no mode:
# the stream still reads, and a close of it
# later closes it once. (freopen closes the stream when it fails, and glibc
# frees it, so a handle left with it was freed twice at its finalizer.)
Dir.mktmpdir("sp_reopen") do |d|
  a = File.join(d, "a")
  File.write(a, "kept\n")
  f = File.open(a)
  begin
    f.reopen(File.join(d, "none"))
  rescue SystemCallError => e
    puts e.class
  end
  puts f.closed?
  begin
    f.reopen(a, "q")
  rescue ArgumentError => e
    puts e.message
  end
  puts f.closed?
  puts f.read
  f.close
  w = File.open(a, "w")
  begin
    w.reopen(File.join(d, "nodir", "x"), "w")
  rescue SystemCallError => e
    puts e.class
  end
  w.write("after")
  w.close
  puts File.read(a)
  # an exclusive mode on a file that exists raises, and the file is not
  # truncated; on a missing one it makes the file
  c = File.join(d, "c")
  File.write(c, "there\n")
  x = File.open(a)
  begin
    x.reopen(c, "wx")
  rescue SystemCallError => e
    puts e.class
  end
  puts File.read(c)
  puts x.closed?
  x.close
  n = File.join(d, "n")
  x = File.open(a)
  x.reopen(n, "wx")
  x.write("made")
  x.close
  puts File.read(n)
  junk = Array.new(50) { |k| "junk #{k}" }
  GC.start
  puts "ok #{junk.size}"
end
