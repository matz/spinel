# spinel: gc-stress
def walk(value, separator)
  result = []
  original = value
  returned = value.each_line(separator, chomp: true) do |piece|
    result << piece
    if result.length == 1
      value.replace("new subject")
      GC.start
    end
  end
  p [returned.equal?(original), result, value]
end

walk(+"a|b|c:d", +"|")

def boxed_lines(pool, separator)
  returned = pool[0].lines(separator) do |piece|
    GC.start
    break piece
  end
  p returned
end

boxed_lines(["head\x00|tail".b, 1], "|".b)
boxed_lines(["", 1], nil)
p "last\r\n".each_line(chomp: true) { |piece| p piece }
