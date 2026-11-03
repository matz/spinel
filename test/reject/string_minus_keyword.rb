# spinel: reject-string-minus
def operand
  raise ArgumentError, "argument evaluated"
end
begin
  "payload".-(amount: operand)
rescue => e
  puts "#{e.class}: #{e.message}"
end
