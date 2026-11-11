# spinel: gc-stress
# #message / #to_s of an exception whose class is known only at run time
# goes through a dispatcher that reads the message the exception owns and
# boxes it. The exception is rooted for the whole dispatch: one built in
# place (`E.new("j").message`) is held by nothing else, and the boxing's
# allocation collected it with its message, so the text read was a freed
# String's (`"i|i"`).
class B < StandardError
  def message = super
end
class C < B; end
class E < StandardError
  def message
    super
  end
end
class T < StandardError
  def to_s = super
end
# an override answering something other than a String: the boxed dispatchers
class KB < StandardError
  def message(&b) = (b ? b.call : super())
end
class KT < StandardError
  def to_s(&b) = (b ? b.call : super())
end

p "#{B.new("i")}|#{E.new("j").message}"
p "#{C.new(+"k")}|#{E.new(+"l").message}|#{T.new("m")}"
p [B.new("x"), E.new("y"), KB.new(+"z")].map(&:message)
p [T.new("t"), KT.new(+"u")].map(&:to_s)
r = []
20.times { |i| r << "#{E.new("e#{i}").message}-#{B.new("b#{i}").message}" }
p r.last, r.uniq.size
begin
  raise C, "raised"
rescue B => e
  p "#{e.message}|#{E.new("after").message}"
end
