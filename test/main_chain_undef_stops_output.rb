# An undef in a module the top level includes stops main's lookup: a bare puts, p, print or format after it raises
# NoMethodError (the arguments run first), at the top level and in a def, and the core's Kernel does not answer; a
# method(:puts) there is a NameError.
module L
  def puts(*a) = $stdout.write("L:#{a.join}\n")
  def p(*a) = $stdout.write("L:p#{a.join}\n")
  def print(*a) = $stdout.write("L:print#{a.join}\n")
  def format(*a) = "L:" + a.join
end
include L
puts "a"
p 1
print "b"
$stdout.write(format("x"), "\n")
module L
  undef_method :puts, :p, :print, :format
end
def arg(n)
  $stdout.write("arg #{n}\n")
  n
end
def in_puts
  puts "z"
rescue NoMethodError => e
  $stdout.write("in def: #{e.class}: #{e.message}\n")
end
def in_p
  p 2
rescue NoMethodError => e
  $stdout.write("in def: #{e.class}: #{e.message}\n")
end
begin
  puts arg(1)
rescue NoMethodError => e
  $stdout.write("#{e.class}: #{e.message}\n")
end
begin
  p arg(2)
rescue NoMethodError => e
  $stdout.write("#{e.class}: #{e.message}\n")
end
begin
  print arg(3)
rescue NoMethodError => e
  $stdout.write("#{e.class}: #{e.message}\n")
end
begin
  format("%d", arg(4))
rescue NoMethodError => e
  $stdout.write("#{e.class}: #{e.message}\n")
end
begin
  method(:puts)
rescue NameError => e
  $stdout.write("#{e.class}: #{e.message[0, 24]}\n")
end
in_puts
in_p
$stdout.write("end\n")
