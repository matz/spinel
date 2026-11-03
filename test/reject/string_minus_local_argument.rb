# spinel: reject-string-minus
value = String.new("payload")
def argument
  raise ArgumentError, "argument evaluated"
end
begin
  value - argument
rescue => e
  puts e.class
end
