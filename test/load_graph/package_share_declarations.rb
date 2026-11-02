# A package's share declarations, in <name>.share.rb beside its file, follow
# the package under both load models: a StringIO reads and writes the
# String it is opened on, answers that String from #string, takes another
# with #string=, and a StringScanner's readers answer Strings of their own.
require "stringio"
require "strscan"

# write-through: the String the StringIO was opened on changes
s = +"ab"
io = StringIO.new(s)
io.write("XY")
p s
io << "Z"
p s
io.puts "!"
p s
p io.string.equal?(s)

# a read answers a new String, not the one the StringIO holds
t = +"hello"
r = StringIO.new(t)
a = r.read(2)
a << "?"
p a, t

# string= keeps the String it is handed
u = +"first"
v = +"second"
w = StringIO.new(u)
w.string = v
w.write("SE")
p u, v, w.string.equal?(v)

# a scanner's readers answer Strings that are not the source
src = +"abc def"
sc = StringScanner.new(src)
x = sc.scan(/abc/)
x << "!"
p x, src
y = sc.rest
y << "?"
p y, src
p sc.string.equal?(src)
