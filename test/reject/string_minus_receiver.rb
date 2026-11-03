# spinel: reject-string-minus
begin
  String.new("payload") - 2
rescue NoMethodError => e
  p e.receiver
end
