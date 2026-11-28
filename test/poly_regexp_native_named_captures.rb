# A boxed receiver's named_captures once strscan loads: StringScanner binds
# the name too, so the call answers a boxed value, the Regexp's or the
# scanner's by the value at run time.
require "strscan"

def captures(x)
  x.named_captures
end

def sent(x, name)
  y = x
  y.public_send(name)
end

s = StringScanner.new("ab")
s.scan(/(?<q>a)/)
p captures([/(?<z>c)/, s][0])
p captures([/(?<z>c)/, s][1])
p sent([/(?<w>d)(?<w>e)/, s][0], :named_captures)
p sent([/(?<w>d)/, s][1], :named_captures)
p sent([/(?<w>d)/, s][0], :source)
begin
  captures([/(?<z>c)/, s, "str"][2])
rescue NoMethodError => e
  puts e.class
end
