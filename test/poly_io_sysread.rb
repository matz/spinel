# IO#sysread on a boxed (poly) IO handle takes the arm readpartial takes:
# the count form, the (count, outbuf) form, EOFError at the end, and the
# count-less and over-long forms refused before the stream is touched.
io = [$stdin, nil][0]
p io.sysread(2)
buf = +"zzzzzzzz"
r = io.sysread(2, buf)
p [r, buf, r.equal?(buf)]
s = +""
s << io.sysread(2)
p s
begin
  io.sysread(4)
rescue EOFError
  puts "eof"
end
begin
  io.sysread
rescue ArgumentError => e
  puts e.message
end
begin
  io.sysread(1, +"b", :extra)
rescue ArgumentError => e
  puts e.message
end

# a parameter that holds a File or a String: the File reads, the String
# answers NoMethodError
def first_bytes(fd)
  buf = ""
  buf += fd.sysread(4)
  buf
end
File.open(__FILE__) { |f| p first_bytes(f) }
begin
  first_bytes("str")
rescue NoMethodError => e
  puts e.message
end
