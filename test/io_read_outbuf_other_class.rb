# The output buffer of read, readpartial, sysread and pread when it is not
# a String: CRuby takes nil as no buffer and any other class through
# StringValue, a TypeError raised after the length converts; a boxed String
# is filled. A longer argument list is ArgumentError. Each of these did not
# build: the arms took every buffer for a String local.
# The length runs, and converts, before the buffer, and a negative one is
# refused there (sysread's and pread's too, since CRuby 4.0.10). A boxed
# String that is shared with another name is filled in place, a frozen one
# raises before the read, and read's at the end of the stream is emptied.
require "tmpdir"

def t
  p yield
rescue TypeError, ArgumentError, FrozenError => e
  p [e.class, e.message]
end

path = File.join(Dir.tmpdir, "spinel_io_read_outbuf_other_#{Process.pid}.txt")
File.write(path, "abcdefgh")
f = File.open(path)
i = 1
sym = :s
arr = [1]
fl = 1.5
nb = nil
t { f.readpartial(2, i) }
t { f.sysread(2, sym) }
t { f.read(2, arr) }
t { f.pread(2, 0, fl) }
t { f.read(2, nb) }
t { f.readpartial(2, nb) }
t { f.pread(2, 1, nb) }
k = ARGV.size
bs = [+"zz", 1][k]
t { f.readpartial(2, bs) }
p bs
bi = [1, +"zz"][k]
t { f.sysread(2, bi) }
p bi
bn = [nil, +"zz"][k]
t { f.read(2, bn) }
t { f.pread(3, 2, bs) }
p bs
t { f.readpartial(1, i, i) }
t { f.sysread(1, nb, nb) }
t { f.pread(1, 0, nb, nb) }
rat = 1r
log = []
t { f.readpartial((log << 1; 2), (log << 2; rat)) }
t { f.sysread((log << 3; 2), (log << 4; arr)) }
p log
neg = -1
t { f.read(neg, i) }
t { f.read(neg, nb) }
t { f.readpartial(neg, sym) }
t { f.sysread(neg, i) }
t { f.pread(neg, 0, arr) }
t { f.read(neg, bi) }
f.rewind
hb = [+"", 1][k]
al = hb
al << "x"
t { f.read(2, hb) }
p [hb, al, hb.equal?(al)]
t { f.readpartial(2, hb) }
p al
fz = [+"", 1][k]
alf = fz
alf << "y"
alf.freeze
t { f.read(2, fz) }
p fz
t { f.read(2, i, i) }
t { STDIN.read(1, i, i) }
t { STDIN.read((log << 5; 1), (log << 6; bs), (log << 7; bs)) }
p log
pf = [f, 1][k]
t { pf.read(1, bs, bs) }
f.read
eb = [+"zz", 1][k]
t { f.read(2, eb) }
p eb
f.close
File.delete(path)
