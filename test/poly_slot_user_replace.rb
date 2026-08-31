# A user method named after a builtin, reached through an UNTYPED slot. The
# evidence that narrowed the slot is the NAME -- `replace` is String's -- so an
# element read out of a heterogeneous array became a string handle and the call
# ran String#replace on the Frag: the receiver's contents became the argument,
# silently. A name a user class owns must not decide an untyped slot's type,
# and the poly dispatch keeps arms for a genuine String/Array receiver so the
# builtin's own answer is unchanged (#4240).
class Frag
  def initialize(html)
    @html = html
  end

  def replace(selector)
    r = yield
    Frag.new("user-replace:" + selector + ":" + r.to_s)
  end

  def to_s
    @html
  end
end

f1 = Frag.new("abc")
puts f1.replace("sel") { nil }.to_s    # typed receiver

box = [Frag.new("abc"), +"just a string"]
f2 = box[0]
puts f2.replace("sel") { nil }.to_s    # untyped slot

s = box[1]                             # a genuine String through the same slot
s.replace("swapped")
puts s
puts box[1]
