# Marshal of an instance of a String subclass (#12162): `I`, `C`, the
# class, the String's bytes, its encoding as the ivar :E and its own ivars,
# as CRuby writes it, and it loads back as an instance of the class with
# its bytes, encoding and ivars. A binary one has no :E, and one with no
# ivars of its own is `I` for the encoding alone.
class Name < String
  attr_accessor :lang
end
class Plain < String
end
n = Name.new("ruby")
n.lang = :ja
d = Marshal.dump(n)
p d
m = Marshal.load(d)
p m, m.class, m.lang, m.encoding, m == n
q = Plain.new("café")
dq = Marshal.dump(q)
p dq
mq = Marshal.load(dq)
p mq, mq.class, mq.encoding, mq.size
b = Plain.new("\xff\x00".b)
db = Marshal.dump(b)
p db
mb = Marshal.load(db)
p mb.bytes, mb.class, mb.encoding
pair = Marshal.load(Marshal.dump([n, n]))
p pair, pair[0].equal?(pair[1]), pair[1].lang
e = Name.new("")
p Marshal.load(Marshal.dump(e)), Marshal.dump(e)
