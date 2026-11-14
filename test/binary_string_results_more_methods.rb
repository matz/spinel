# BINARY receivers and arguments answer BINARY results, read by bytes: getc,
# a socket's gets, strftime, -@, reverse, tr and dump.
require "stringio"
require "socket"
io = StringIO.new("\xFF\xD8ab\n".b)
p io.getc.encoding
p io.readchar.encoding
a, b = UNIXSocket.pair
a.write("x\xFF\n".b)
a.write("y\xFF\n".b)
p b.gets.encoding
p b.readline.encoding
p Time.at(0).utc.strftime("%Y\xFF".b).encoding
p((-"\xFF\x00".b).encoding)
p "日本".b.reverse
p "ab".tr("b", "\xFF".b)
p "\xFF\xD8\x00ab".b.dump
x = "ab".tr("b", "\xFF".b); p x, x.encoding
begin; y = "日b".tr("b", "\xFF".b); p y, y.encoding; rescue => e; p e; end
d = "\xFF\xD8\x00ab".b.dump; p d, d.encoding
