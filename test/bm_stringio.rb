require "stringio"

# Basic write operations
s = StringIO.new
s.puts "hello"
s.print "world"
s.write "!"
puts s.string
puts s.pos
puts s.size

# Rewind and read
s.rewind
puts s.read
puts s.pos

# Initialize with string and read lines
s2 = StringIO.new("abc\ndef\nghi")
puts s2.gets
puts s2.gets
puts s2.eof?
puts s2.gets
puts s2.eof?

# Seek and tell
s3 = StringIO.new("hello world")
s3.seek(6)
puts s3.read
puts s3.tell

# Truncate
s4 = StringIO.new
s4.write "hello world"
s4.truncate(5)
puts s4.string

# putc
s5 = StringIO.new
s5.putc(65)
s5.putc(66)
puts s5.string

# Rewind, read with length
s6 = StringIO.new("abcdefghij")
puts s6.read(5)
puts s6.read(3)

# eof? and close
s7 = StringIO.new("x")
puts s7.eof?
s7.read
puts s7.eof?
s7.close
puts s7.closed?

# Empty puts
s8 = StringIO.new
s8.puts
s8.puts "line2"
puts s8.string.length

# getc and getbyte
s9 = StringIO.new("AB")
puts s9.getc
puts s9.getbyte

# flush, sync, isatty
s10 = StringIO.new
s10.flush
puts s10.sync
puts s10.isatty

# StringIO stored in an instance variable
class StringIOHolder
  def initialize
    @io = StringIO.new("abc")
  end

  def value
    @io.string
  end
end

puts StringIOHolder.new.value
