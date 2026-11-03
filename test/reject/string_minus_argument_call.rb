# spinel: reject-string-minus
AMOUNT = 2
begin
  ::String.new("payload").send("-", AMOUNT + 1)
rescue NoMethodError => e
  p [e.class, e.name, e.receiver, e.args]
end
