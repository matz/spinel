# Transpose reports the actual class of a row that cannot convert to Array.

class StringRow < String
end

begin
  [[1], "text"].transpose
rescue TypeError => e
  puts e.message
end

begin
  [[1], StringRow.new("text")].transpose
rescue TypeError => e
  puts e.message
end
