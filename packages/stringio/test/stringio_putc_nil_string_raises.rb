# StringIO#putc with a String-or-nil argument that is nil raises
# The [:string] binding took the nil (NULL) as an empty String: it wrote
# nothing and answered nil, where Ruby raises TypeError, as it does for a
# literal nil or a boxed one (the [:int] binding's path). Directly and
# through a dispatch over the receiver's class.
require "stringio"

def try
  p yield
rescue TypeError => e
  p [:type_error, e.message]
end

def s_or_nil(x) = x ? "AB" : nil

io = StringIO.new
try { io.putc(s_or_nil(ARGV.size > 5)) }
try { io.putc(s_or_nil(ARGV.size < 5)) }
try { io.putc(nil) }
try { io.putc(67) }
p io.string

class W
  def putc(s) = "w:#{s.inspect}"
end
io2 = StringIO.new
[io2, W.new].each do |o|
  try { o.putc(s_or_nil(ARGV.size > 5)) }
  try { o.putc(s_or_nil(ARGV.size < 5)) }
end
p io2.string
