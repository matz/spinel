# A nil where an Integer is wanted raises TypeError worded by the
# conversion CRuby makes there: an IO offset (NUM2OFFT: seek, sysseek,
# pos=, truncate, the offset of pread and pwrite) says "from nil" where off_t
# is wider than a long (macOS, a 32-bit build) and "from nil to integer" where
# it is a long (Linux x86-64), so the offset lines drop a trailing " to integer";
# Random#bytes' size "of nil into Integer", and pread's and readpartial's
# length (NUM2LONG) "from nil to integer" (pread's since CRuby 4.0.10). The nil may be
# written, held in a mixed Array, an Integer slot's nil (a String#index
# miss), or the receiver itself held in a mixed Array.

require "tmpdir"

def show(tag)
  p yield
rescue => e
  msg = e.message
  msg = msg.sub(/ to integer\z/, "") if tag =~ /off|seek|pos=|truncate|miss/
  puts "#{tag} #{e.class}: #{msg}"
end

def t(k)
  path = File.join(Dir.tmpdir, "io_offset_nil_wording_#{Process.pid}.txt")
  File.write(path, "hello")
  f = File.open(path, "r+")
  b = [f, 1][k]
  nn = [nil, 1][k]
  show("pread len") { f.pread(nil, 0) }
  show("pread off") { f.pread(2, nil) }
  show("pread boxed len") { b.pread(nil, 0) }
  show("pread boxed off") { b.pread(2, nil) }
  show("pread nil len") { f.pread(nn, 0) }
  show("pread nil off") { f.pread(2, nn) }
  show("pread") { f.pread(2, 1) }
  show("pwrite off") { f.pwrite("a", nil) }
  show("pwrite boxed off") { b.pwrite("a", nil) }
  show("pwrite nil off") { f.pwrite("a", nn) }
  show("seek") { f.seek(nil) }
  show("seek boxed") { b.seek(nil) }
  show("seek nil") { f.seek(nn) }
  show("sysseek") { f.sysseek(nil) }
  show("sysseek boxed") { b.sysseek(nil) }
  show("pos=") { f.pos = nil }
  show("pos= boxed") { b.pos = nil }
  show("truncate") { f.truncate(nil) }
  show("truncate nil") { f.truncate(nn) }
  miss = "abc".index("z")
  show("seek miss") { f.seek(miss) }
  show("pread miss") { f.pread(2, miss) }
  show("readpartial") { f.readpartial(nil) }
  show("bytes") { Random.new(1).bytes(nn) }
  f.close
  File.delete(path)
end
t(ARGV.size)
