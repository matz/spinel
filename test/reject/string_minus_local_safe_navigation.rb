# spinel: reject-string-minus
value = String.new("payload")
amount = 2
begin
  value&.-(amount)
rescue NoMethodError => e
  p e.receiver
end
