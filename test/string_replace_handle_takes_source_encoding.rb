# spinel: share
# String#replace on a String held as a handle (a local, an instance variable,
# the value of the call) takes the encoding of its argument with its bytes:
# a buffer made by String.new turns text, a text string turns binary. The
# boxed receiver is test/string_replace_takes_source_encoding.rb.
class Holder
  def initialize = @buf = String.new(capacity: 8)

  def take(s)
    @buf.replace(s)
    @buf
  end
end

def show(label, s) = puts("#{label}: #{s.encoding} #{s.inspect} #{s.bytesize}")

buf = String.new(capacity: 16)
buf.replace("São")
show("buffer takes text", buf)

text = +"abc"
text.replace("São".b)
show("text takes binary", text)

text.replace("ok")
show("then takes text again", text)

show("ivar", Holder.new.take("ção"))

val = String.new(capacity: 4)
got = val.replace("ünï")
show("value of the call", got)
puts "returns self: #{got.equal?(val)}"

poly = [String.new(capacity: 4), 42].first
poly.replace("ïñ")
show("boxed receiver", poly)

empty = +"x"
empty.replace("".b)
show("empty binary", empty)
