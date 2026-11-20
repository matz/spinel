subject = +"ab"
original = subject
result = subject.each_char do |character|
  subject = +"new"
  GC.start
  break :stopped if character == "z"
end
result << "!"
p [result.equal?(original), original, subject]

def scan(flag)
  subject = flag ? :other : +"ab"
  original = subject
  result = subject.scan(/./) do |match|
    subject = +"changed"
    GC.start
    break :stopped if match == "z"
  end
  result << "!"
  p [result.equal?(original), original, subject]
end
scan(ARGV.size > 0)

subject = +"ab"
result = subject.each_byte do |byte|
  subject = +"changed"
  break :stopped if byte == 98
end
p [result, subject]
