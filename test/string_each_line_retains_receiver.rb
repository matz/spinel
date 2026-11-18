def pick(values, i) = values[i]

class LineSubject < String
end

source = +"a|b"
separator = +"|"
typed = source.each_line(separator)
boxed = pick([source, 7], 0).each_line(separator, chomp: true)
p [typed.class, boxed.class, typed.size, boxed.size]
source.replace("first/second")
separator.replace("/")
p typed.to_a, boxed.to_a
source.replace("next:end")
separator.replace(":")
p typed.to_a, boxed.to_a

empty = LineSubject.new("")
same = empty.each_line(nil, chomp: true)
first = same.next
p [first.class, first.equal?(empty), first.empty?]
begin
  same.next
rescue StopIteration => e
  p e.result.equal?(empty)
end
empty << "later"
p same.to_a[0].equal?(empty), same.to_a[0]
p "a\r\nb\n".each_line("\n", chomp: true).to_a
p "\x80\0\x81".b.each_line("\0", chomp: true).to_a.map { |x| [x.bytes, x.encoding.name] }
